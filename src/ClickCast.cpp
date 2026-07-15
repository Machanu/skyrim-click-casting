#include "ClickCast.h"

#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

// ============================================================================
// Click-Casting core — single-hand latch / auto-release, plus symmetric dual-latch.
//
// Threading: the AttackBlockHandler::ProcessButton and PlayerCharacter::Update hooks
// both run on the game's main thread, so the latch state below is only ever touched
// from one thread — no synchronisation is required.
//
// How dual casting actually reaches this code, as established by in-game diagnostic logs:
// the engine never emits a single "Dual Attack" button event on keyboard/mouse — a dual
// always arrives as two separate Left and Right events. During the dual the engine drives
// ONLY ONE caster (in practice the LEFT) to kReady while the partner caster stays kNone.
// See the "driving-pair mode" block below for how that is handled. The single-event path is
// still implemented for completeness, but never fires in practice.
// ============================================================================

namespace
{
    using ProcessButtonFn = void(RE::AttackBlockHandler*, RE::ButtonEvent*, RE::PlayerControlsData*);

    // Original ProcessButton, captured by the vtable hook.
    REL::Relocation<ProcessButtonFn> g_originalProcessButton;

    bool g_installed = false;

    // Hand indices.
    enum : std::size_t
    {
        kLeft = 0,
        kRight = 1,
        kHandCount = 2
    };

    // Latch lifecycle. Releasing is entered AFTER the synthetic release is delivered: it
    // holds the hand inert during the brief post-release firing window (empirically the
    // caster's kUnk(4) phase, ~100-200 ms) so a click there cannot reach vanilla and cancel
    // the just-fired cast. It ends on caster wind-down (kNone), an equipped-object change,
    // or a 1.5 s safety timeout.
    enum class Phase
    {
        Idle,
        Latched,
        Releasing
    };

    // Charge-start grace. Some equip configs (same castable spell in both hands) let the engine
    // DEFER charge start while it decides whether a dual is forming, so the caster reads kNone
    // for the first frames after latch. kNone is therefore treated as an interrupt ONLY after
    // charge has been observed to start; until then the latch is held for up to this long
    // (accumulated frame delta, never wall-clock — same discipline as the Releasing safety).
    constexpr float kChargeStartGraceSecs = 1.0f;

    // Hold-to-recast pacing. After a firing completes (Releasing->Idle) with
    // the button still held, wait this long before starting the next latch — a short breather so
    // holding does not spam casts. Accumulated via the Update hook's a_delta (never wall-clock).
    constexpr float kRecastDelaySecs = 0.25f;

    // Pair stall guard. If the driving-pair driver sticks in its first charging state
    // (kUnk(1)) without progressing for this long, the engine has wedged: vanilla holds a down
    // that will never release. Abort the pair, clean up, and go Idle. a_delta-accumulated.
    constexpr float kPairStallSecs = 2.0f;

    struct HandLatch
    {
        Phase                   phase = Phase::Idle;
        RE::TESForm*            spell = nullptr;                          // latched equipped object (spell/scroll/staff) for change detection
        RE::INPUT_DEVICE        device = RE::INPUT_DEVICE::kKeyboard;     // for the synthetic event
        RE::BSFixedString       userEvent;                               // "Left/Right Attack/Block" (per-hand single release)
        std::uint32_t           idCode = 0;
        RE::PlayerControlsData* data = nullptr;                          // captured for synthetic delivery
        RE::MagicCaster::State  lastState = RE::MagicCaster::State::kNone; // for state-change logging
        float                   releasingElapsed = 0.0f;                 // seconds accumulated in Releasing
        bool                    dualEventHand = false;                   // latched as part of a single "Dual Attack" pair
        bool                    lastDualCasting = false;                 // last GetIsDualCasting() (for change-only logging)
        bool                    chargeStarted = false;                   // caster observed != kNone at least once since latch
        float                   graceElapsed = 0.0f;                     // seconds waited for charge to start
        bool                    graceLogged = false;                     // "waiting for charge start" logged once
        bool                    adopted = false;                         // polling the PARTNER caster as driver (two-handed form)
        // Logging-only (no decision reads these): measured time since latch, and the gate's
        // charge time — so the compact latch/fire lines can carry ct= and t=.
        float                   latchElapsed = 0.0f;
        float                   chargeTime = 0.0f;
    };

    HandLatch g_hand[kHandCount];

    // ---- dual-pair coordination -------------------------------------------
    // Two ways a dual could arise:
    //   * DualEvent: ONE physical "Dual Attack" button-down latches BOTH hands, then waits
    //     until BOTH casters are kReady and delivers a single synthetic "Dual Attack"
    //     release. The engine never emits such an event on keyboard/mouse, so this path is
    //     implemented but does not fire in practice.
    //   * TwoEvent: independent Left + Right latches happen to be active at the same time.
    //     Each hand keeps its own single-hand release; the two virtual holds ARE the
    //     symmetric dual. The pair is only a marker.
    enum class PairKind
    {
        None,
        DualEvent,
        TwoEvent
    };

    struct DualPair
    {
        PairKind                kind = PairKind::None;
        // Payload of the swallowed "Dual Attack" down, reused for the single dual release.
        RE::INPUT_DEVICE        device = RE::INPUT_DEVICE::kKeyboard;
        RE::BSFixedString       userEvent;                               // "Dual Attack"
        std::uint32_t           idCode = 0;
        RE::PlayerControlsData* data = nullptr;
    };

    DualPair g_pair;

    void ClearPair()
    {
        g_pair.kind = PairKind::None;
        g_pair.device = RE::INPUT_DEVICE::kKeyboard;
        g_pair.userEvent = RE::BSFixedString();
        g_pair.idCode = 0;
        g_pair.data = nullptr;
    }

    // ---- driving-pair mode -------------------------------------------------
    // In-game diagnostics established that a dual cast arrives as separate Left+Right events,
    // and that during the dual the engine drives ONLY ONE caster (in practice the LEFT) to
    // kReady while the PARTNER caster sits at kNone the whole time. A naive per-hand poll
    // reads that expected partner-kNone as "charge interrupted" and resets the partner latch,
    // after which the partner's physical button-up reaches vanilla and cancels the dual charge.
    //
    // Fix: once a pair is active we STOP polling per hand. We poll only the driving caster,
    // treat the partner's kNone as expected (never reset on it), and when the driver reaches
    // kReady we synthesize a release for BOTH hands. Both hands stay in per-hand phase Latched
    // (then Releasing) so the existing hook keeps swallowing their physical events unchanged.
    struct DrivePair
    {
        bool        active = false;
        std::size_t driver = kLeft;           // hand whose caster the engine actually drives
        bool        released = false;         // both synthetic releases delivered -> Releasing
        float       releasingElapsed = 0.0f;  // seconds accumulated in the pair Releasing window
        bool        chargeStarted = false;    // driver caster observed != kNone at least once
        float       graceElapsed = 0.0f;      // seconds waited for the driver's charge to start
        bool        graceLogged = false;      // "waiting for charge start" logged once
        std::uint32_t firstChargeState = 0;   // first non-kNone driver state after charge start (0 = unset)
        float       stallElapsed = 0.0f;      // seconds stuck in the first charging state (stall guard)
        float       pairElapsed = 0.0f;       // logging-only: seconds since the pair activated
    };

    DrivePair g_drive;

    // ---- physical button tracking (hold-to-recast) ------------------------
    // The PHYSICAL state of each hand's attack button (down seen, up not yet seen), tracked in
    // the hook SEPARATELY from latch state. Physical ups are swallowed under latch/Releasing, so
    // the up is recorded BEFORE it is swallowed. When a hand's Releasing phase ends (cast done)
    // while its button is still held and the gate still passes, a fresh latch is started as if
    // re-clicked. device/idCode/data mirror the originating physical down so a synthetic down can
    // be replayed to vanilla to start the new charge (exactly what a real click's pass-through
    // does). A hand can be Idle while its button is still physically held — hence this is not a
    // field of HandLatch.
    struct PhysicalButton
    {
        bool                    down = false;
        RE::INPUT_DEVICE        device = RE::INPUT_DEVICE::kKeyboard;
        std::uint32_t           idCode = 0;
        RE::PlayerControlsData* data = nullptr;
    };

    PhysicalButton g_phys[kHandCount];

    // ---- pending recast (hold-to-recast 0.25 s delay) ---------------------
    // Between a completed firing (Releasing->Idle, button still held) and the next latch, a
    // countdown runs on the Update hook's a_delta. On expiry the button-held + gate check is
    // re-evaluated and, if still valid, a fresh latch starts. Releasing the button during the
    // wait cancels it; a fresh physical click during the wait clears it (the click latches
    // normally). One per hand — a dual just schedules both.
    struct PendingRecast
    {
        bool  active = false;
        float remaining = 0.0f;
    };

    PendingRecast g_pending[kHandCount];

    // ---- small helpers -----------------------------------------------------

    const char* HandName(std::size_t a_hand) noexcept
    {
        return a_hand == kLeft ? "Left" : "Right";
    }

    const char* StateName(RE::MagicCaster::State a_state) noexcept
    {
        using S = RE::MagicCaster::State;
        switch (a_state) {
        case S::kNone:     return "kNone";
        case S::kReady:    return "kReady";
        case S::kCharging: return "kCharging";
        case S::kCasting:  return "kCasting";
        default:           return "kUnk";
        }
    }

    RE::MagicSystem::CastingSource CasterSource(std::size_t a_hand) noexcept
    {
        return a_hand == kLeft ? RE::MagicSystem::CastingSource::kLeftHand
                               : RE::MagicSystem::CastingSource::kRightHand;
    }

    RE::MagicCaster* GetCaster(RE::PlayerCharacter* a_pc, std::size_t a_hand)
    {
        return a_pc ? a_pc->GetMagicCaster(CasterSource(a_hand)) : nullptr;
    }

    void CallOriginal(RE::AttackBlockHandler* a_this, RE::ButtonEvent* a_event, RE::PlayerControlsData* a_data)
    {
        if (g_originalProcessButton.address() != 0) {
            g_originalProcessButton(a_this, a_event, a_data);
        }
    }

    void ClearLatch(std::size_t a_hand)  // silent reset of all latch fields
    {
        auto& L = g_hand[a_hand];
        L.phase = Phase::Idle;
        L.spell = nullptr;
        L.data = nullptr;
        L.idCode = 0;
        L.userEvent = RE::BSFixedString();
        L.lastState = RE::MagicCaster::State::kNone;
        L.releasingElapsed = 0.0f;
        L.dualEventHand = false;
        L.lastDualCasting = false;
        L.chargeStarted = false;
        L.graceElapsed = 0.0f;
        L.graceLogged = false;
        L.adopted = false;
        L.latchElapsed = 0.0f;
        L.chargeTime = 0.0f;

        // A TwoEvent pair is only a log marker over two independent single-hand latches;
        // once either hand clears, the marker no longer applies. (A DualEvent pair manages
        // g_pair explicitly in the poll, so never clear that kind here.)
        if (g_pair.kind == PairKind::TwoEvent) {
            g_pair.kind = PairKind::None;
        }
    }

    // Every reset is a deviation (the latch did NOT complete normally), so it always logs its
    // reason plus how far the charge had got.
    void ResetLatch(std::size_t a_hand, const char* a_reason)
    {
        auto& L = g_hand[a_hand];
        if (L.phase != Phase::Idle) {
            spdlog::info("[cc] reset {} @{:.2f}s state={} -> Idle ({})",
                HandName(a_hand), L.latchElapsed, StateName(L.lastState), a_reason);
        }
        ClearLatch(a_hand);
    }

    bool IsStaffForm(const RE::TESForm* a_form) noexcept
    {
        if (!a_form || a_form->GetFormType() != RE::FormType::Weapon) {
            return false;
        }
        return static_cast<const RE::TESObjectWEAP*>(a_form)->IsStaff();
    }

    // What kind of castable the latched equipped object is — for the compact log lines only; the
    // machine treats spell / scroll / staff identically.
    const char* KindName(const RE::TESForm* a_form) noexcept
    {
        if (!a_form) {
            return "none";
        }
        switch (a_form->GetFormType()) {
        case RE::FormType::Scroll: return "scroll";
        case RE::FormType::Weapon: return IsStaffForm(a_form) ? "staff" : "weapon";
        default:                   return "spell";
        }
    }

    const char* CastingTypeName(RE::MagicSystem::CastingType a_ct) noexcept
    {
        using C = RE::MagicSystem::CastingType;
        switch (a_ct) {
        case C::kConstantEffect: return "constant-effect";
        case C::kFireAndForget:  return "fire-and-forget";
        case C::kConcentration:  return "concentration";
        case C::kScroll:         return "scroll";
        default:                 return "unknown";
        }
    }

    // Two-handed shared-form signature: the PARTNER hand has the SAME equipped form as
    // a_hand's latch AND its caster is actively casting (state != kNone). In that config the
    // engine runs ONE cast on the partner caster, so a release on a_hand's control would cancel
    // that running cast. Used both to trigger partner-driver adoption and to guard cleanup.
    bool PartnerActiveSameForm(std::size_t a_hand, RE::PlayerCharacter* a_pc)
    {
        if (!a_pc || !g_hand[a_hand].spell) {
            return false;
        }
        const std::size_t partner = (a_hand == kLeft) ? kRight : kLeft;
        auto*      pCaster = GetCaster(a_pc, partner);
        const auto pState  = pCaster ? pCaster->state.get() : RE::MagicCaster::State::kNone;
        if (pState == RE::MagicCaster::State::kNone) {
            return false;
        }
        RE::TESForm* partnerForm = a_pc->GetEquippedObject(partner == kLeft);
        return partnerForm == g_hand[a_hand].spell;
    }

    // Pass-through dedup: the form for which a "[cc] pass-through" line was last emitted per hand.
    // A gate rejection is logged once per equipped form (the pointer IS the equip identity), so
    // holding/clicking a non-castable does not spam the log.
    RE::TESForm* g_ptLogged[kHandCount] = { nullptr, nullptr };


    // ---- gate --------------------------------------------------------------
    // Returns the qualifying equipped OBJECT for the hand (SpellItem, ScrollItem, or a staff
    // TESObjectWEAP), or nullptr, logging the deciding criterion. The object is carried as a
    // TESForm* so the whole latch / release / recast / dual machine can treat spell, scroll and
    // staff identically — L.spell is only used for equipped-object identity + diagnostics, never
    // for SpellItem-specific calls. FaF requirement (castingType == kFireAndForget, chargeTime
    // > 0) is evaluated on the SpellItem for spells/scrolls and on the ENCHANTMENT for staves.
    // A gate rejection is a pass-through: log it ONCE per equipped form per hand (the form pointer
    // is the equip identity), so repeated clicks on a non-castable stay quiet.
    void LogPassThrough(std::size_t a_hand, RE::TESForm* a_form, const char* a_name, const char* a_reason)
    {
        if (g_ptLogged[a_hand] == a_form) {
            return;
        }
        g_ptLogged[a_hand] = a_form;
        if (a_name) {
            spdlog::info("[cc] pass-through {} {}='{}' ({})", HandName(a_hand), KindName(a_form), a_name, a_reason);
        } else {
            spdlog::info("[cc] pass-through {} ({})", HandName(a_hand), a_reason);
        }
    }

    // a_outChargeTime (optional) receives the qualifying charge time, so the caller's single
    // "[cc] latch" line can carry it — the gate itself emits no PASS line.
    RE::TESForm* EvaluateGate(RE::PlayerCharacter* a_pc, std::size_t a_hand, float* a_outChargeTime = nullptr)
    {
        if (!a_pc) {
            spdlog::error("[cc] gate {} FAIL: no player singleton", HandName(a_hand));
            return nullptr;
        }

        RE::TESForm* form = a_pc->GetEquippedObject(a_hand == kLeft);
        if (!form) {
            LogPassThrough(a_hand, nullptr, nullptr, "empty hand");
            return nullptr;
        }

        const RE::FormType ft = form->GetFormType();

        // ---- Spell / Scroll: the equipped object IS a SpellItem (ScrollItem : SpellItem). ----
        if (ft == RE::FormType::Spell || ft == RE::FormType::Scroll) {
            auto* spell = static_cast<RE::SpellItem*>(form);

            const auto castingType = spell->GetCastingType();
            if (castingType != RE::MagicSystem::CastingType::kFireAndForget) {
                LogPassThrough(a_hand, form, spell->GetName(), CastingTypeName(castingType));
                return nullptr;
            }
            const float chargeTime = spell->GetChargeTime();
            if (!(chargeTime > 0.0f)) {
                LogPassThrough(a_hand, form, spell->GetName(), "ct<=0 (instant)");
                return nullptr;
            }
            if (a_outChargeTime) {
                *a_outChargeTime = chargeTime;
            }
            g_ptLogged[a_hand] = nullptr;  // it qualifies now; a later rejection may log again
            return form;
        }

        // ---- Staff: Weapon + IsStaff + FaF enchantment + chargeTime>0. In-game testing on a
        // heavily modded setup showed a FaF staff drives the hand caster through the SAME
        // kUnk->kReady sequence as a spell, so the machine is reused 1:1 with the staff weapon
        // as L.spell.
        if (ft == RE::FormType::Weapon) {
            auto* weap = static_cast<RE::TESObjectWEAP*>(form);
            if (!weap->IsStaff()) {
                LogPassThrough(a_hand, form, weap->GetName(), "not a staff");
                return nullptr;
            }
            RE::EnchantmentItem* ench = weap->formEnchanting;
            if (!ench) {
                LogPassThrough(a_hand, form, weap->GetName(), "no enchantment");
                return nullptr;
            }
            const auto castingType = ench->GetCastingType();
            if (castingType != RE::MagicSystem::CastingType::kFireAndForget) {
                LogPassThrough(a_hand, form, weap->GetName(), CastingTypeName(castingType));
                return nullptr;
            }
            const float chargeTime = ench->GetChargeTime();
            if (!(chargeTime > 0.0f)) {
                LogPassThrough(a_hand, form, weap->GetName(), "ct<=0 (instant)");
                return nullptr;
            }
            if (a_outChargeTime) {
                *a_outChargeTime = chargeTime;
            }
            g_ptLogged[a_hand] = nullptr;
            return form;  // the STAFF WEAPON is the tracked equipped object
        }

        // ---- Everything else. ----
        LogPassThrough(a_hand, form, form->GetName(), "not spell/scroll/staff");
        return nullptr;
    }

    // ---- synthetic release -------------------------------------------------
    // Construct a release ButtonEvent (value=0, heldDownSecs>0) that mirrors a latched
    // control and hand it straight to the original ProcessButton. Shared by the single-hand
    // release and the coordinated dual release.
    void DeliverSyntheticRelease(RE::INPUT_DEVICE a_device, const RE::BSFixedString& a_userEvent,
                                 std::uint32_t a_idCode, RE::PlayerControlsData* a_data, const char* a_tag)
    {
        auto* controls = RE::PlayerControls::GetSingleton();
        RE::AttackBlockHandler* handler = controls ? controls->attackBlockHandler : nullptr;
        if (!handler || !a_data || g_originalProcessButton.address() == 0) {
            spdlog::error("[cc] ERROR {}: cannot deliver synthetic release (handler={}, data={}, orig={})",
                a_tag,
                static_cast<const void*>(handler),
                static_cast<const void*>(a_data),
                (g_originalProcessButton.address() != 0 ? "ok" : "null"));
            return;
        }

        constexpr float kSyntheticHeld = 0.1f;  // any value > 0 marks this as a release, not a fresh down
        RE::ButtonEvent* ev = RE::ButtonEvent::Create(a_device, a_userEvent, a_idCode, 0.0f, kSyntheticHeld);
        if (!ev) {
            spdlog::error("[cc] ERROR {}: ButtonEvent::Create (release) returned null", a_tag);
            return;
        }

        // Delivery itself is implicit in the caller's fire/recast/cleanup line — no log here.
        g_originalProcessButton(handler, ev, a_data);

        // We own this event (delivered directly, not inserted into the game's event list).
        ev->~ButtonEvent();
        RE::free(ev);
    }

    // Single-hand release, using the hand's own latched Left/Right control.
    void SynthesizeRelease(std::size_t a_hand)
    {
        auto& L = g_hand[a_hand];
        DeliverSyntheticRelease(L.device, L.userEvent, L.idCode, L.data, HandName(a_hand));
    }

    // Coordinated dual release: one "Dual Attack" up, mirroring the swallowed dual down.
    void SynthesizeDualRelease()
    {
        DeliverSyntheticRelease(g_pair.device, g_pair.userEvent, g_pair.idCode, g_pair.data, "Dual pair");
    }

    // ---- cleanup release ---------------------------------------------------
    // A latch/pair is ending WITHOUT a synthetic release having been delivered (kNone-interrupt
    // before kReady, grace timeout, equip-change, stall). Vanilla received a down for this hand
    // and, if the button is now physically UP, is left holding it forever (a hanging hold).
    // Deliver ONE synthetic release to balance it. The cast is already dead, so cancellation is
    // the intended outcome — the "no release before kReady" rule only guards ACTIVE casts. Never
    // clean up while the button is still physically held: vanilla's hold is then legitimate and
    // the eventual physical up (Idle pass-through) will balance it. Call BEFORE the latch fields
    // are cleared. Uses the hand's current latch payload.
    //
    // Mutually exclusive with the hold-to-recast synthetic up: cleanup fires only from a
    // Latched/Driving -> Idle end (no release delivered) with the button UP, whereas the recast
    // up fires only from a Releasing -> Idle end (release delivered) with the button HELD. Same
    // hold can never take both, so vanilla never receives two ups for one down.
    void CleanupReleaseIfNeeded(std::size_t a_hand, const char* a_reason)
    {
        if (g_phys[a_hand].down) {
            spdlog::info("[cc] cleanup {} skipped (btn still held) ({})", HandName(a_hand), a_reason);
            return;
        }
        if (!g_hand[a_hand].data) {
            return;  // nothing captured to balance against
        }
        spdlog::info("[cc] cleanup {} (btn up, no release delivered) ({})", HandName(a_hand), a_reason);
        SynthesizeRelease(a_hand);
    }

    // ---- hold-to-recast ----------------------------------------------------
    // Deliver a synthetic button-DOWN (value=1, heldDownSecs=0) straight to the original
    // ProcessButton, so vanilla begins charging the hand's caster — exactly what a real click's
    // pass-through does. A DOWN is not a release, so this never violates the "no release before
    // kReady" rule. Uses the latch fields just populated by PerformRecast.
    void DeliverSyntheticDown(std::size_t a_hand)
    {
        auto& L = g_hand[a_hand];
        auto* controls = RE::PlayerControls::GetSingleton();
        RE::AttackBlockHandler* handler = controls ? controls->attackBlockHandler : nullptr;
        if (!handler || !L.data || g_originalProcessButton.address() == 0) {
            spdlog::error("[cc] ERROR {} hand: cannot deliver synthetic down (handler={}, data={}, orig={})",
                HandName(a_hand), static_cast<const void*>(handler), static_cast<const void*>(L.data),
                (g_originalProcessButton.address() != 0 ? "ok" : "null"));
            return;
        }
        constexpr float kFreshDown = 0.0f;  // heldDownSecs==0 marks a fresh press, not a hold/up
        RE::ButtonEvent* ev = RE::ButtonEvent::Create(L.device, L.userEvent, L.idCode, 1.0f, kFreshDown);
        if (!ev) {
            spdlog::error("[cc] ERROR {} hand: ButtonEvent::Create (down) returned null", HandName(a_hand));
            return;
        }
        g_originalProcessButton(handler, ev, L.data);
        ev->~ButtonEvent();
        RE::free(ev);
    }

    // Deliver a synthetic UP (value=0) using the hand's tracked PHYSICAL-button payload (the
    // latch is already cleared at Releasing->Idle, but g_phys persists). This tells vanilla the
    // attack button is released, balancing its hold and STOPPING the engine's own auto-recharge
    // that otherwise begins the instant the kReady release fires (because the physical key is
    // still down). Delivered only when the button is physically held; a click already released
    // and must NOT receive this. Logged under [recast].
    void DeliverRecastUp(std::size_t a_hand)
    {
        auto& P = g_phys[a_hand];
        auto* userEvents = RE::UserEvents::GetSingleton();
        const RE::BSFixedString ue = userEvents
            ? (a_hand == kLeft ? userEvents->leftAttack : userEvents->rightAttack)
            : RE::BSFixedString();

        auto* controls = RE::PlayerControls::GetSingleton();
        RE::AttackBlockHandler* handler = controls ? controls->attackBlockHandler : nullptr;
        if (!handler || !P.data || g_originalProcessButton.address() == 0) {
            spdlog::error("[cc] ERROR {} hand: cannot deliver synthetic up (handler={}, data={}, orig={})",
                HandName(a_hand), static_cast<const void*>(handler), static_cast<const void*>(P.data),
                (g_originalProcessButton.address() != 0 ? "ok" : "null"));
            return;
        }
        constexpr float kSyntheticHeld = 0.1f;  // value=0 + heldDownSecs>0 = a release/up
        RE::ButtonEvent* ev = RE::ButtonEvent::Create(P.device, ue, P.idCode, 0.0f, kSyntheticHeld);
        if (!ev) {
            spdlog::error("[cc] ERROR {} hand: ButtonEvent::Create (up) returned null", HandName(a_hand));
            return;
        }
        // Delivery is implicit in the "[cc] recast ... armed" line — no log here.
        g_originalProcessButton(handler, ev, P.data);
        ev->~ButtonEvent();
        RE::free(ev);
    }

    // Perform the actual recast (called when a pending recast's 0.25 s delay elapses): if the
    // hand's attack button is still physically held AND the gate still passes, start a fresh
    // latch — a synthetic down begins the new charge and the normal poll delivers the release at
    // kReady. Every decision is logged with [recast]. The hand must be Idle.
    bool PerformRecast(std::size_t a_hand, RE::PlayerCharacter* a_pc, const char* a_transition)
    {
        if (g_hand[a_hand].phase != Phase::Idle) {
            spdlog::info("[cc] recast {} skip (no longer Idle) [{}]", HandName(a_hand), a_transition);
            return false;
        }
        auto& P = g_phys[a_hand];
        if (!P.down) {
            spdlog::info("[cc] recast {} skip (btn not held) [{}]", HandName(a_hand), a_transition);
            return false;
        }
        if (P.device == RE::INPUT_DEVICE::kGamepad) {
            spdlog::info("[cc] recast {} skip (gamepad)", HandName(a_hand));
            return false;
        }
        if (!a_pc || !P.data) {
            spdlog::info("[cc] recast {} skip (no player/controls data)", HandName(a_hand));
            return false;
        }

        // Full gate re-evaluation on the CURRENT equip (a rejection logs one pass-through line).
        float        chargeTime = 0.0f;
        RE::TESForm* spell      = EvaluateGate(a_pc, a_hand, &chargeTime);
        if (!spell) {
            spdlog::info("[cc] recast {} skip (gate reject)", HandName(a_hand));
            return false;
        }

        // Start a fresh latch, mirroring a new click (payload from the tracked physical button).
        auto* userEvents = RE::UserEvents::GetSingleton();
        auto* caster = GetCaster(a_pc, a_hand);
        auto& L = g_hand[a_hand];
        L.phase = Phase::Latched;
        L.spell = spell;
        L.device = P.device;
        L.userEvent = userEvents ? (a_hand == kLeft ? userEvents->leftAttack : userEvents->rightAttack)
                                 : RE::BSFixedString();
        L.idCode = P.idCode;
        L.data = P.data;
        L.lastState = caster ? caster->state.get() : RE::MagicCaster::State::kNone;
        L.releasingElapsed = 0.0f;
        L.dualEventHand = false;
        L.lastDualCasting = caster ? caster->GetIsDualCasting() : false;
        L.chargeStarted = false;
        L.graceElapsed = 0.0f;
        L.graceLogged = false;
        L.latchElapsed = 0.0f;
        L.chargeTime = chargeTime;

        // The relatch itself is implicit — the preceding "recast ... armed" line and the following
        // "fire" line bracket it, so a hold cycle stays at two lines.

        // Feed vanilla a fresh down so it begins charging (a click's pass-through equivalent).
        DeliverSyntheticDown(a_hand);
        return true;
    }

    // Schedule a pending recast at a Releasing->Idle transition: if the button is still held,
    // arm the 0.25 s delay; the actual relatch happens on expiry (PerformRecast). If the button
    // is not held, there is nothing to recast.
    void SchedulePendingRecast(std::size_t a_hand, const char* a_transition)
    {
        if (!g_phys[a_hand].down) {
            g_pending[a_hand].active = false;
            return;  // plain click: nothing to recast — no line (the "fire" line already closed it)
        }
        // Button still held: the firing is done, but vanilla will auto-recharge because the key
        // is physically down. Deliver ONE synthetic UP to stop that, then arm the delay — the mod
        // now owns the recast tempo. The physical up is swallowed while pending (no double up).
        DeliverRecastUp(a_hand);
        g_pending[a_hand].active = true;
        g_pending[a_hand].remaining = kRecastDelaySecs;
        spdlog::info("[cc] recast {} armed(+{:.2f}s)", HandName(a_hand), kRecastDelaySecs);
    }

    // Schedule pending recasts for both hands (a dual that just fired). On expiry each relatches
    // independently; the existing GetIsDualCasting detection re-forms the driving pair once the
    // engine dual-charges again.
    void SchedulePendingRecastPair(const char* a_transition)
    {
        SchedulePendingRecast(kLeft, a_transition);
        SchedulePendingRecast(kRight, a_transition);
    }

    // One hand of a DualEvent pair dropped out (kNone / equip change / lost caster). Resolve
    // the partner so no "dead hand" is left latched: if it already reached kReady,
    // release it as a single hand; otherwise let it continue as a plain single-hand latch that
    // self-releases at its own kReady. Either way the dual pair is dissolved.
    void HandleDualPairBreak(std::size_t a_fallenHand, RE::PlayerCharacter* a_pc)
    {
        const std::size_t other = (a_fallenHand == kLeft) ? kRight : kLeft;
        auto& O = g_hand[other];

        if (O.phase == Phase::Latched && O.dualEventHand) {
            auto*      caster = GetCaster(a_pc, other);
            const auto oState = caster ? caster->state.get() : RE::MagicCaster::State::kNone;
            if (oState == RE::MagicCaster::State::kReady) {
                spdlog::info("[cc] pair asymmetric-fall: {} dropped, {} kReady -> single-hand release",
                    HandName(a_fallenHand), HandName(other));
                SynthesizeRelease(other);  // uses O.userEvent (Left/Right)
                O.dualEventHand = false;
                O.phase = Phase::Releasing;
                O.releasingElapsed = 0.0f;
            } else {
                spdlog::info("[cc] pair asymmetric-fall: {} dropped, {} still charging -> single-hand latch",
                    HandName(a_fallenHand), HandName(other));
                O.dualEventHand = false;  // now a plain single-hand latch
            }
        }

        ClearPair();
    }

    // Reset one hand during the poll; if it was part of a DualEvent pair, resolve the partner.
    void ResetDuringPoll(std::size_t a_hand, RE::PlayerCharacter* a_pc, const char* a_reason)
    {
        // A Latched hand has NOT had its release delivered yet (that happens only at kReady ->
        // Releasing). If its button is physically up, balance vanilla's hold (cleanup release).
        // But NOT when the partner runs the same-form cast — a release on this control would
        // cancel that running cast. Suppress and log.
        if (g_hand[a_hand].phase == Phase::Latched) {
            if (PartnerActiveSameForm(a_hand, a_pc)) {
                spdlog::info("[cc] cleanup {} suppressed (partner casting same form) ({})",
                    HandName(a_hand), a_reason);
            } else {
                CleanupReleaseIfNeeded(a_hand, a_reason);
            }
        }
        const bool wasDual = g_hand[a_hand].dualEventHand;
        ResetLatch(a_hand, a_reason);
        if (wasDual && a_pc) {
            HandleDualPairBreak(a_hand, a_pc);
        }
    }

    // ---- per-frame poll (driven by the PlayerCharacter::Update hook) --------
    // Runs at most once per frame per latched hand. There is NO self-rescheduling:
    // the update hook is the sole driver. (The previous self-rescheduling SKSE
    // AddTask chain re-ran within a single frame and froze the main thread.)
    void PollOnce(std::size_t a_hand, float a_delta)
    {
        auto& L = g_hand[a_hand];
        if (L.phase == Phase::Idle) {   // nothing to do
            return;
        }

        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) {
            ResetDuringPoll(a_hand, nullptr, "no player");
            return;
        }

        // Safety (both phases): spell swapped / unequipped / sheathed away in this hand.
        RE::TESForm* form = pc->GetEquippedObject(a_hand == kLeft);
        if (form != L.spell) {
            const bool wasReleasing = (L.phase == Phase::Releasing);
            ResetDuringPoll(a_hand, pc, "equip-change (swap/unequip/sheathe)");
            if (wasReleasing) {  // Releasing->Idle: hold-to-recast may re-latch the new equip
                SchedulePendingRecast(a_hand, "Releasing->Idle (equip-change)");
            }
            return;
        }

        auto* caster = GetCaster(pc, a_hand);
        if (!caster) {
            ResetDuringPoll(a_hand, pc, "no magic caster");
            return;
        }

        // Track the caster state (used by the deviation lines) and the elapsed charge time (used
        // by the single fire line). Normal kUnk(1)->kUnk(2)->kReady progression is NOT logged.
        const auto state = caster->state.get();
        L.lastState = state;
        L.latchElapsed += a_delta;

        // Charge-start grace: mark once the caster has actually left kNone.
        if (state != RE::MagicCaster::State::kNone) {
            L.chargeStarted = true;
        }

        // Partner-driver adoption: a two-handed scroll runs its cast on ONLY
        // ONE caster. If, while Latched in grace, this hand's own caster stays kNone but the
        // PARTNER holds the same form with an active caster, adopt the partner as this latch's
        // driver — poll it and release THIS hand at the partner's kReady. Generic mechanism; only
        // expected to trigger for two-handed scrolls. Never for DualEvent hands (own path).
        if (L.phase == Phase::Latched && !L.adopted && !L.dualEventHand && !L.chargeStarted &&
            state == RE::MagicCaster::State::kNone && PartnerActiveSameForm(a_hand, pc)) {
            const std::size_t partner = (a_hand == kLeft) ? kRight : kLeft;
            auto*      pC = GetCaster(pc, partner);
            const auto pS = pC ? pC->state.get() : RE::MagicCaster::State::kNone;
            L.adopted = true;
            L.chargeStarted = true;  // the (partner's) charge HAS started -> grace no longer applies
            spdlog::info("[cc] adopt {}->{} driver (grace, partner {} active, form match '{}')",
                HandName(a_hand), HandName(partner), StateName(pS), L.spell ? L.spell->GetName() : "?");
        }

        // The caster whose state drives this latch's timing: own, or the adopted partner. For a
        // non-adopted hand this is exactly its own caster, so spell behaviour is unchanged.
        const std::size_t driverHand  = L.adopted ? ((a_hand == kLeft) ? kRight : kLeft) : a_hand;
        auto*             driveCaster = L.adopted ? GetCaster(pc, driverHand) : caster;
        const auto        driveState  = driveCaster ? driveCaster->state.get() : RE::MagicCaster::State::kNone;

        if (L.phase == Phase::Releasing) {
            // Post-release firing window. Time source: the update hook's per-frame delta
            // (seconds), already flowing through here and naturally paused in menus.
            L.releasingElapsed += a_delta;

            // Exit (1): the driving caster wound down -> the cast completed normally. This is the
            // expected end of a cast and needs no line; the "fire" line already reported it.
            if (driveState == RE::MagicCaster::State::kNone) {
                ClearLatch(a_hand);
                SchedulePendingRecast(a_hand, "Releasing->Idle (cast completed)");
                return;
            }

            // Exit (3): safety timeout — a deviation (the caster never wound down).
            if (L.releasingElapsed > 1.5f) {
                spdlog::info("[cc] safety {} Releasing timeout @1.5s state={} -> Idle",
                    HandName(a_hand), StateName(driveState));
                ClearLatch(a_hand);
                SchedulePendingRecast(a_hand, "Releasing->Idle (safety timeout)");
                return;
            }

            // Exit (2) (equipped-object change) is handled by the shared safety check above.
            return;  // stay in Releasing
        }

        // ---- phase == Latched ----

        // Safety reset (no synthetic release) — charge ended / was interrupted. For a dual
        // pair this is the asymmetric-fall case: resolve the partner.
        if (driveState == RE::MagicCaster::State::kNone) {
            if (!L.chargeStarted) {
                // Charge hasn't started yet (engine's dual-decision window). Hold the latch and
                // keep swallowing events; do NOT treat this as an interrupt.
                L.graceElapsed += a_delta;
                L.graceLogged = true;  // grace itself is silent unless it times out (below)
                if (L.graceElapsed > kChargeStartGraceSecs) {
                    ResetDuringPoll(a_hand, pc, "grace timeout: charge never started");
                }
                return;
            }
            ResetDuringPoll(a_hand, pc,
                L.adopted ? "interrupt: adopted driver kNone before kReady"
                          : "interrupt: kNone before kReady (charge aborted)");
            return;
        }

        // kReady is treated as charge-complete -> fire, then hold in Releasing so the
        // post-release firing window cannot be cancelled by a stray click.
        if (driveState == RE::MagicCaster::State::kReady) {
            if (L.dualEventHand) {
                // DualEvent coordination: only release once BOTH casters are ready.
                const std::size_t other  = (a_hand == kLeft) ? kRight : kLeft;
                auto&             O       = g_hand[other];
                auto*             oCaster = GetCaster(pc, other);
                const auto        oState  = oCaster ? oCaster->state.get() : RE::MagicCaster::State::kNone;

                if (O.phase == Phase::Latched && O.dualEventHand && oState == RE::MagicCaster::State::kReady) {
                    // Both ready -> ONE coordinated "Dual Attack" release; both -> Releasing.
                    spdlog::info("[cc] pair fire @{:.2f} (dual-event, both kReady)", L.latchElapsed);
                    SynthesizeDualRelease();
                    for (const std::size_t h : { kLeft, kRight }) {
                        g_hand[h].phase = Phase::Releasing;
                        g_hand[h].releasingElapsed = 0.0f;
                        g_hand[h].dualEventHand = false;
                    }
                    ClearPair();
                } else if (!(O.phase == Phase::Latched && O.dualEventHand)) {
                    // Partner no longer paired (dropped/downgraded) -> fire this hand alone.
                    spdlog::info("[cc] fire {} @{:.2f} (partner unpaired -> single-hand)",
                        HandName(a_hand), L.latchElapsed);
                    SynthesizeRelease(a_hand);
                    L.dualEventHand = false;
                    L.phase = Phase::Releasing;
                    L.releasingElapsed = 0.0f;
                    ClearPair();
                }
                // else: partner still charging -> wait (stay Latched at kReady, no per-frame log).
                return;
            }

            // The one lifecycle line for a firing: it subsumes the whole normal kUnk(1) ->
            // kUnk(2) -> kReady progression that is no longer logged per transition.
            spdlog::info("[cc] fire {} t={:.2f} (kReady->release{})",
                HandName(a_hand), L.latchElapsed, L.adopted ? ", adopted driver" : "");
            SynthesizeRelease(a_hand);  // release the LATCHED hand (whose button was clicked)
            L.phase = Phase::Releasing;
            L.releasingElapsed = 0.0f;
            return;
        }

        // Still charging — the update hook polls again next frame.
    }

    // ---- driving-pair helpers ----------------------------------------------

    // End the pair (break before release, or clean-up after Releasing). Both hands -> Idle so
    // no dead hand is left latched; the detection marker and driving state are cleared.
    void EndPair(const char* a_reason)
    {
        // If the pair is ending BEFORE its release was delivered, each hand's vanilla hold is
        // unbalanced — clean up (release if the hand's button is physically up). After release
        // (g_drive.released) the Releasing exits handle recast instead.
        if (!g_drive.released) {
            for (const std::size_t h : { kLeft, kRight }) {
                if (g_hand[h].phase == Phase::Latched) {
                    CleanupReleaseIfNeeded(h, a_reason);
                }
            }
        }

        // A pair end after its release is the normal close of a dual (already reported by the
        // "pair fire" line) — only a break BEFORE the release is a deviation worth a line.
        if (!g_drive.released) {
            spdlog::info("[cc] pair break @{:.2f}s -> both Idle ({})", g_drive.pairElapsed, a_reason);
        }
        ClearLatch(kLeft);
        ClearLatch(kRight);
        ClearPair();
        g_drive.pairElapsed = 0.0f;
        g_drive.active = false;
        g_drive.driver = kLeft;
        g_drive.released = false;
        g_drive.releasingElapsed = 0.0f;
        g_drive.chargeStarted = false;
        g_drive.graceElapsed = 0.0f;
        g_drive.graceLogged = false;
        g_drive.firstChargeState = 0;
        g_drive.stallElapsed = 0.0f;
    }

    // Latch a partner hand that has not latched on its own, so a driving pair can cover both
    // hands (activation route (b): GetIsDualCasting() revealed a dual while only one hand had
    // latched). No physical ButtonEvent exists to mirror, so the release payload is synthesized:
    // the hand's own Left/Right control string, the driver's device/data, idCode 0 (vanilla
    // matches releases on the control string). Returns false if the hand cannot be gated.
    bool LatchPartnerForPair(std::size_t a_hand, RE::PlayerCharacter* a_pc, RE::INPUT_DEVICE a_device,
                             RE::PlayerControlsData* a_data)
    {
        RE::TESForm* spell = EvaluateGate(a_pc, a_hand);
        if (!spell || !a_data) {
            return false;
        }
        auto* userEvents = RE::UserEvents::GetSingleton();
        auto* caster = GetCaster(a_pc, a_hand);
        auto& L = g_hand[a_hand];
        L.phase = Phase::Latched;
        L.spell = spell;
        L.device = a_device;
        L.userEvent = userEvents ? (a_hand == kLeft ? userEvents->leftAttack : userEvents->rightAttack)
                                 : RE::BSFixedString();
        L.idCode = 0;
        L.data = a_data;
        L.lastState = caster ? caster->state.get() : RE::MagicCaster::State::kNone;
        L.releasingElapsed = 0.0f;
        L.dualEventHand = false;
        L.lastDualCasting = caster ? caster->GetIsDualCasting() : false;
        // Force-latching the partner is part of forming the pair — the "[cc] pair" line reports it.
        return true;
    }

    // Upgrade to a driving pair ONLY when a real engine dual is observed: GetIsDualCasting()==true
    // on a caster while at least one hand is Latched. Both-latched alone stays two INDEPENDENT
    // latches (handled by per-hand PollOnce). Called BEFORE any per-hand poll so that, once a real
    // dual is active, the partner's expected kNone can never reset it.
    void UpdateDrivePairActivation(RE::PlayerCharacter* a_pc)
    {
        if (g_drive.active || !a_pc) {
            return;
        }
        // The single-event DualEvent path coordinates itself — leave those hands to it.
        if (g_hand[kLeft].dualEventHand || g_hand[kRight].dualEventHand) {
            return;
        }

        const bool lLatched = g_hand[kLeft].phase == Phase::Latched;
        const bool rLatched = g_hand[kRight].phase == Phase::Latched;
        if (!lLatched && !rLatched) {
            return;  // nothing latched -> no pair possible
        }
        // Don't hijack a hand that is mid-Releasing into a fresh pair.
        if (g_hand[kLeft].phase == Phase::Releasing || g_hand[kRight].phase == Phase::Releasing) {
            return;
        }

        auto*      lc     = GetCaster(a_pc, kLeft);
        auto*      rc     = GetCaster(a_pc, kRight);
        const bool lDual  = lc && lc->GetIsDualCasting();
        const bool rDual  = rc && rc->GetIsDualCasting();
        const auto lState = lc ? lc->state.get() : RE::MagicCaster::State::kNone;
        const auto rState = rc ? rc->state.get() : RE::MagicCaster::State::kNone;

        // A driving pair forms ONLY on a real engine dual cast, i.e. GetIsDualCasting()==true on
        // a caster. Both-latched alone is NOT a dual: with different spells per hand the engine
        // charges both casters INDEPENDENTLY (both state != kNone, GetIsDualCasting()==false) and
        // each hand must fire at its OWN kReady — that is handled by per-hand PollOnce, not here.
        // Releasing both at the driver's kReady would cancel the partner's still-charging cast.
        if (!lDual && !rDual) {
            return;
        }

        // Pick the driving hand: the caster the engine actually drives. Prefer the hand whose
        // caster reports dual-casting; else the hand that is charging while the other is kNone;
        // else default Left (empirically the engine drives the left caster).
        using S = RE::MagicCaster::State;
        std::size_t driver = kLeft;
        if (lDual && !rDual)                                   driver = kLeft;
        else if (rDual && !lDual)                              driver = kRight;
        else if (lState != S::kNone && rState == S::kNone)     driver = kLeft;
        else if (rState != S::kNone && lState == S::kNone)     driver = kRight;
        else                                                   driver = kLeft;

        // Both hands must be latched so both physical events stay swallowed and both can be
        // released. Force-latch a missing partner (its button is down: we are dual-casting).
        RE::INPUT_DEVICE        device = lLatched ? g_hand[kLeft].device : g_hand[kRight].device;
        RE::PlayerControlsData* data   = lLatched ? g_hand[kLeft].data   : g_hand[kRight].data;
        if (g_hand[kLeft].phase == Phase::Idle && !LatchPartnerForPair(kLeft, a_pc, device, data)) {
            return;
        }
        if (g_hand[kRight].phase == Phase::Idle && !LatchPartnerForPair(kRight, a_pc, device, data)) {
            return;
        }

        g_drive.active = true;
        g_drive.driver = driver;
        g_drive.released = false;
        g_drive.releasingElapsed = 0.0f;
        // Grace starts fresh: a pair activated via the dualcasting trigger can have driver
        // state == kNone (charge not yet started); the first PollDrivePair will honour grace.
        g_drive.chargeStarted = (driver == kLeft ? lState : rState) != RE::MagicCaster::State::kNone;
        g_drive.graceElapsed = 0.0f;
        g_drive.graceLogged = false;
        g_drive.firstChargeState = 0;
        g_drive.stallElapsed = 0.0f;
        g_drive.pairElapsed = 0.0f;

        RE::TESForm* dForm = g_hand[driver].spell;
        spdlog::info("[cc] pair L+R {}='{}' (engine dual, driver={})",
            KindName(dForm), dForm ? dForm->GetName() : "?", HandName(driver));
    }

    // Poll a driving pair: ONLY the driving caster decides state; the partner's kNone is
    // expected and never resets anything. Both hands are released at the driver's kReady.
    void PollDrivePair(RE::PlayerCharacter* a_pc, float a_delta)
    {
        if (!a_pc) {
            EndPair("no player");
            return;
        }

        // Equip-change on EITHER hand ends the pair (Releasing exit / real break) -> both Idle.
        for (const std::size_t h : { kLeft, kRight }) {
            RE::TESForm* form = a_pc->GetEquippedObject(h == kLeft);
            if (form != g_hand[h].spell) {
                const bool wasReleasing = g_drive.released;
                EndPair("equip-change (swap/unequip/sheathe)");
                if (wasReleasing) {  // Releasing->Idle: hold-to-recast for held hands
                    SchedulePendingRecastPair("pair Releasing->Idle (equip-change)");
                }
                return;
            }
        }

        const std::size_t drv     = g_drive.driver;
        auto*             dCaster = GetCaster(a_pc, drv);
        if (!dCaster) {
            EndPair("no driver magic caster");
            return;
        }
        const auto dState = dCaster->state.get();

        // Track driver state + pair age for the deviation / fire lines. Normal progression is
        // not logged per transition.
        g_hand[drv].lastState = dState;
        g_drive.pairElapsed += a_delta;

        // Charge-start grace: mark once the driver caster has actually left kNone.
        if (dState != RE::MagicCaster::State::kNone) {
            g_drive.chargeStarted = true;
        }

        // Stall guard: while Driving, the driver must progress past its FIRST charging
        // state (kUnk(1)). If it sticks there for kPairStallSecs it has wedged (never reaches
        // kUnk(2)) — abort the pair with cleanup and no recast. A normal charge leaves kUnk(1)
        // within ~14 ms, so the later dwell in kUnk(2) never counts here.
        if (!g_drive.released && g_drive.chargeStarted &&
            dState != RE::MagicCaster::State::kNone && dState != RE::MagicCaster::State::kReady) {
            const std::uint32_t sv = static_cast<std::uint32_t>(dState);
            if (g_drive.firstChargeState == 0) {
                g_drive.firstChargeState = sv;
                g_drive.stallElapsed = 0.0f;
            }
            if (sv == g_drive.firstChargeState) {
                g_drive.stallElapsed += a_delta;
                if (g_drive.stallElapsed > kPairStallSecs) {
                    spdlog::info("[cc] stall pair @{:.1f}s driver={} stuck state={} -> abort",
                        kPairStallSecs, HandName(drv), sv);
                    EndPair("stall guard: driver stuck in first charging state");
                    return;
                }
            } else {
                g_drive.stallElapsed = 0.0f;  // progressed past kUnk(1) -> normal charge
            }
        }

        // ---- pair Releasing window (post-fire) ----
        if (g_drive.released) {
            g_drive.releasingElapsed += a_delta;
            if (dState == RE::MagicCaster::State::kNone) {
                EndPair("driver wound down (dual cast completed)");
                SchedulePendingRecastPair("pair Releasing->Idle (cast completed)");
                return;
            }
            if (g_drive.releasingElapsed > 1.5f) {
                EndPair("Releasing timeout (1.5s)");
                SchedulePendingRecastPair("pair Releasing->Idle (safety timeout)");
                return;
            }
            return;  // stay Releasing; the hook swallows both hands' physical events
        }

        // ---- pair Driving ----
        if (dState == RE::MagicCaster::State::kNone) {
            if (!g_drive.chargeStarted) {
                // Driver charge hasn't started yet (dual-decision window). Hold the pair; do NOT
                // treat this as an interrupt (both hands keep swallowing).
                g_drive.graceElapsed += a_delta;
                g_drive.graceLogged = true;  // grace itself is silent unless it times out (below)
                if (g_drive.graceElapsed > kChargeStartGraceSecs) {
                    EndPair("grace timeout: driver charge never started");
                }
                return;
            }
            // Driver fell to kNone BEFORE kReady after charge had started -> genuine interrupt.
            EndPair("interrupt: driver kNone before kReady");
            return;
        }

        if (dState == RE::MagicCaster::State::kReady) {
            const std::size_t other = (drv == kLeft) ? kRight : kLeft;
            spdlog::info("[cc] pair fire @{:.2f} (driver={} kReady->release both)",
                g_drive.pairElapsed, HandName(drv));
            SynthesizeRelease(drv);    // driving hand first
            SynthesizeRelease(other);
            for (const std::size_t h : { kLeft, kRight }) {
                g_hand[h].phase = Phase::Releasing;
                g_hand[h].releasingElapsed = 0.0f;
            }
            g_drive.released = true;
            g_drive.releasingElapsed = 0.0f;
            return;
        }

        // Driver still charging -> the update hook polls again next frame.
    }

    // ---- pending-recast countdown ------------------------------------------
    // Tick each hand's pending recast on the Update hook's a_delta. Cancel a hand whose button
    // was released during the delay. On expiry perform the recast (re-checks button-held + gate).
    // When BOTH hands are pending it is a dual: they expire TOGETHER and both synthetic downs go
    // out in the SAME frame (deterministic order, left first) so the engine gets a clean
    // simultaneous dual-detection instead of a drifted, asynchronous re-latch.
    void TickPendingRecasts(RE::PlayerCharacter* a_pc, float a_delta)
    {
        // Cancel any hand released during its delay, then decrement the survivors.
        for (std::size_t hand = 0; hand < kHandCount; ++hand) {
            auto& PR = g_pending[hand];
            if (!PR.active) {
                continue;
            }
            if (!g_phys[hand].down) {  // released during the delay -> cancel
                spdlog::info("[cc] recast {} cancel (btn released)", HandName(hand));
                PR.active = false;
                continue;
            }
            PR.remaining -= a_delta;
        }

        const bool lPending = g_pending[kLeft].active;
        const bool rPending = g_pending[kRight].active;

        if (lPending && rPending) {
            // Dual pending: fire both together when EITHER has elapsed (they are armed in the
            // same frame with identical deltas, so this is deterministic and synchronized).
            if (g_pending[kLeft].remaining <= 0.0f || g_pending[kRight].remaining <= 0.0f) {
                g_pending[kLeft].active = false;
                g_pending[kRight].active = false;
                PerformRecast(kLeft, a_pc, "hold-to-recast pair (0.25s delay elapsed)");   // left first
                PerformRecast(kRight, a_pc, "hold-to-recast pair (0.25s delay elapsed)");
            }
            return;
        }

        // Single pending hand.
        for (const std::size_t hand : { kLeft, kRight }) {
            auto& PR = g_pending[hand];
            if (PR.active && PR.remaining <= 0.0f) {
                PR.active = false;
                PerformRecast(hand, a_pc, "hold-to-recast (0.25s delay elapsed)");
            }
        }
    }

    // ---- per-frame update hook ---------------------------------------------
    // PlayerCharacter::Update (Actor vtable index 0xAD, RE\A\Actor.h:367) runs once per
    // frame. We poll each latched hand exactly once, then ALWAYS chain the original.
    // This replaces the old self-rescheduling SKSE task chain.
    using UpdateFn = void(RE::PlayerCharacter*, float);
    REL::Relocation<UpdateFn> g_originalUpdate;

    void Update_Hook(RE::PlayerCharacter* a_this, float a_delta)
    {
        auto* pc = RE::PlayerCharacter::GetSingleton();

        // Hold-to-recast delay: expire pending recasts BEFORE pairing/polling, so any relatch
        // (and its synthetic down) is in place before a driving pair can re-form.
        TickPendingRecasts(pc, a_delta);

        // Form/upgrade a driving pair BEFORE any per-hand poll, so the partner hand is never
        // reset by its own expected kNone during an engine dual cast.
        if (!g_drive.active) {
            UpdateDrivePairActivation(pc);
        }

        if (g_drive.active) {
            PollDrivePair(pc, a_delta);  // pair mode: poll only the driving caster
        } else {
            for (std::size_t hand = 0; hand < kHandCount; ++hand) {
                if (g_hand[hand].phase != Phase::Idle) {   // poll Latched and Releasing hands
                    PollOnce(hand, a_delta);
                }
            }
        }

        g_originalUpdate(a_this, a_delta);  // always chain the original update
    }

    // ---- dual-attack control (single-event path) ---------------------------
    // Latch one hand as part of a "Dual Attack" pair. Stores the hand's OWN Left/Right
    // control string (for a possible single-hand fallback release on asymmetric fall); the
    // coordinated dual release uses the "Dual Attack" payload held in g_pair.
    void LatchDualHand(std::size_t a_hand, RE::TESForm* a_spell, RE::PlayerCharacter* a_pc,
                       RE::ButtonEvent* a_event, RE::PlayerControlsData* a_data)
    {
        auto* userEvents = RE::UserEvents::GetSingleton();
        auto* caster = GetCaster(a_pc, a_hand);
        auto& L = g_hand[a_hand];

        L.phase = Phase::Latched;
        L.spell = a_spell;
        L.device = a_event->device.get();
        L.userEvent = userEvents ? (a_hand == kLeft ? userEvents->leftAttack : userEvents->rightAttack)
                                 : RE::BSFixedString();
        L.idCode = a_event->GetIDCode();
        L.data = a_data;
        L.lastState = caster ? caster->state.get() : RE::MagicCaster::State::kNone;
        L.releasingElapsed = 0.0f;
        L.dualEventHand = true;
        L.lastDualCasting = caster ? caster->GetIsDualCasting() : false;
    }

    // Handle a "Dual Attack" control event (already classified; gamepad already excluded).
    void ProcessDualControl(RE::AttackBlockHandler* a_this, RE::ButtonEvent* a_event,
                            RE::PlayerControlsData* a_data, const RE::BSFixedString& a_ue,
                            bool a_isDown, bool a_isUp)
    {
        const bool pairLatched     = (g_pair.kind == PairKind::DualEvent);
        const bool eitherReleasing = (g_hand[kLeft].phase == Phase::Releasing) ||
                                     (g_hand[kRight].phase == Phase::Releasing);

        if (a_isDown) {
            if (pairLatched) {  // duplicate dual-down during latch -> ignore (no cancel-click)
                return;         // swallowing during a latch is normal — no line
            }

            auto*        pc = RE::PlayerCharacter::GetSingleton();
            RE::TESForm* ls = EvaluateGate(pc, kLeft);
            RE::TESForm* rs = EvaluateGate(pc, kRight);

            const bool bothIdle = g_hand[kLeft].phase == Phase::Idle && g_hand[kRight].phase == Phase::Idle;
            if (ls && rs && bothIdle) {
                LatchDualHand(kLeft, ls, pc, a_event, a_data);
                LatchDualHand(kRight, rs, pc, a_event, a_data);

                g_pair.kind = PairKind::DualEvent;
                g_pair.device = a_event->device.get();
                g_pair.userEvent = a_ue;  // "Dual Attack"
                g_pair.idCode = a_event->GetIDCode();
                g_pair.data = a_data;

                spdlog::info("[cc] pair L+R L='{}' R='{}' (dual-attack event)",
                    ls->GetName(), rs->GetName());

                // Pass the dual-DOWN to vanilla so it begins charging BOTH casters: swallowing
                // it entirely would leave vanilla with nothing to charge. This mirrors the
                // single-hand latch, which also passes its own down through — charge on down,
                // swallow only the dual-UP virtual hold. The payload is captured above.
                CallOriginal(a_this, a_event, a_data);
                return;
            }

            // Gate not satisfied for BOTH hands (or a hand already busy) -> full pass-through.
            // The per-hand gate already emitted its deduped pass-through line.
            CallOriginal(a_this, a_event, a_data);
            return;
        }

        if (a_isUp) {
            if (pairLatched || eitherReleasing) {  // virtual hold: swallow the physical dual-up
                return;  // normal virtual hold — no line
            }
            CallOriginal(a_this, a_event, a_data);
            return;
        }

        // Held continuation.
        if (pairLatched) {
            CallOriginal(a_this, a_event, a_data);  // pass through so vanilla keeps charging
            return;
        }
        if (eitherReleasing) {
            return;  // swallow held during the post-release window
        }
        CallOriginal(a_this, a_event, a_data);
    }

    // ---- the hook ----------------------------------------------------------
    void ProcessButton_Hook(RE::AttackBlockHandler* a_this, RE::ButtonEvent* a_event, RE::PlayerControlsData* a_data)
    {
        if (!a_event) {
            CallOriginal(a_this, a_event, a_data);
            return;
        }

        // Which attack control is this? Classify against Left / Right / Dual.
        auto* userEvents = RE::UserEvents::GetSingleton();
        const RE::BSFixedString& ue = a_event->QUserEvent();

        std::size_t hand          = kHandCount;  // kLeft / kRight for a single-hand control
        bool        isDualControl = false;
        const char* controlLabel  = "other";
        if (userEvents) {
            if (ue == userEvents->rightAttack) {
                hand = kRight;
                controlLabel = "RightAttack";
            } else if (ue == userEvents->leftAttack) {
                hand = kLeft;
                controlLabel = "LeftAttack";
            } else if (ue == userEvents->dualAttack) {
                isDualControl = true;
                controlLabel = "DualAttack";
            }
        }

        if (hand == kHandCount && !isDualControl) {
            CallOriginal(a_this, a_event, a_data);  // not an attack control -> pass-through
            return;
        }

        const bool isDown = a_event->value > 0.0f && a_event->heldDownSecs == 0.0f;
        const bool isUp   = a_event->value == 0.0f && a_event->heldDownSecs > 0.0f;

        // Individual attack events are not logged: a normal click is fully described by its
        // "latch" + "fire" pair, and deviations carry their own line.

        // Gamepad is pass-through (spec). If somehow latched, leave it be; only swallow M+KB.
        if (a_event->device.get() == RE::INPUT_DEVICE::kGamepad) {
            CallOriginal(a_this, a_event, a_data);
            return;
        }

        // Hold-to-recast: record the M+KB physical button state for this hand BEFORE any swallow,
        // so a swallowed up is still observed. Independent of latch phase.
        if (hand != kHandCount) {
            if (isDown) {
                g_phys[hand].down = true;
                g_phys[hand].device = a_event->device.get();
                g_phys[hand].idCode = a_event->GetIDCode();
                g_phys[hand].data = a_data;
                // A fresh physical click supersedes any pending recast for this hand; the click
                // latches normally below (its own "latch" line reports it).
                g_pending[hand].active = false;
            } else if (isUp) {
                g_phys[hand].down = false;
            }
        }

        // During a pending recast the synthetic UP has ALREADY been delivered to vanilla, so the
        // physical up/held for this hand must be swallowed — passing it through would be a second
        // up (double release). A fresh physical DOWN is allowed through: it cleared the pending
        // above and latches normally below.
        if (hand != kHandCount && g_pending[hand].active && !isDown) {
            return;  // normal during a pending recast — the cancel/fire line reports the outcome
        }

        // The "Dual Attack" control has its own handling.
        if (isDualControl) {
            ProcessDualControl(a_this, a_event, a_data, ue, isDown, isUp);
            return;
        }

        auto& L = g_hand[hand];

        // Releasing: swallow ALL physical events for this hand (before gate evaluation) so a
        // click in the post-release firing window cannot reach vanilla and cancel the cast.
        if (L.phase == Phase::Releasing) {
            return;  // swallow down/up/held — the normal post-fire window, no line
        }

        if (L.phase == Phase::Latched) {
            if (isUp) {
                // Swallow the physical button-up — this is the "virtual hold" (normal, no line).
                return;  // do NOT call original -> vanilla never sees the release
            }
            if (isDown) {
                return;  // second click during charge is ignored (spec: no cancel-click)
            }
            // Held continuation (value>0, heldDownSecs>0) -> pass through so vanilla keeps charging.
            CallOriginal(a_this, a_event, a_data);
            return;
        }

        // Idle. Only a fresh button-down can start a latch.
        if (isDown) {
            auto*        pc         = RE::PlayerCharacter::GetSingleton();
            float        chargeTime = 0.0f;
            RE::TESForm* spell      = EvaluateGate(pc, hand, &chargeTime);
            if (spell) {
                auto* caster = GetCaster(pc, hand);

                L.phase = Phase::Latched;
                L.spell = spell;
                L.device = a_event->device.get();
                L.userEvent = ue;
                L.idCode = a_event->GetIDCode();
                L.data = a_data;
                L.lastState = caster ? caster->state.get() : RE::MagicCaster::State::kNone;
                L.releasingElapsed = 0.0f;
                L.dualEventHand = false;  // independent single-hand latch, not a dual-event pair
                L.lastDualCasting = caster ? caster->GetIsDualCasting() : false;
                L.latchElapsed = 0.0f;
                L.chargeTime = chargeTime;

                // The one lifecycle line for a latch — the gate PASS is implicit in it.
                spdlog::info("[cc] latch {} {}='{}' ct={:.2f}",
                    HandName(hand), KindName(spell), spell->GetName(), chargeTime);

                // Both hands latched is NOT a dual by itself (different spells charge
                // independently); only a later GetIsDualCasting() upgrades them to a driving
                // pair, which logs its own "pair" line. Nothing to report here.

                // Let vanilla see the button-DOWN so it begins charging normally.
                CallOriginal(a_this, a_event, a_data);

                // The PlayerCharacter::Update hook now polls this hand once per frame.
                return;
            }
            // Gate failed -> untouched pass-through.
            CallOriginal(a_this, a_event, a_data);
            return;
        }

        // Any other event while idle (held/up with no latch) -> pass-through.
        CallOriginal(a_this, a_event, a_data);
    }
}

namespace ClickCast
{
    void Install()
    {
        if (g_installed) {
            spdlog::info("[cc] install: already installed; skipping");
            return;
        }

        // Hook AttackBlockHandler::ProcessButton (vtable slot 4). Use the concrete
        // VTABLE_AttackBlockHandler offset array — AttackBlockHandler has no own VTABLE
        // member and would otherwise resolve to the base HeldStateHandler vtable.
        REL::Relocation<std::uintptr_t> vtblAttack{ RE::VTABLE_AttackBlockHandler[0] };
        g_originalProcessButton = vtblAttack.write_vfunc(0x4, &ProcessButton_Hook);

        // Per-frame driver: hook PlayerCharacter::Update (Actor vtable index 0xAD,
        // RE\A\Actor.h:367; VTABLE_PlayerCharacter[0], RE\Offsets_VTABLE.h:2231). Polls
        // each latched hand exactly once per frame — replaces the self-rescheduling
        // SKSE task chain that froze the main thread.
        REL::Relocation<std::uintptr_t> vtblPlayer{ RE::VTABLE_PlayerCharacter[0] };
        g_originalUpdate = vtblPlayer.write_vfunc(0xAD, &Update_Hook);

        g_installed = true;

        spdlog::info("[cc] install: hooked AttackBlockHandler::ProcessButton (vtbl 4) + PlayerCharacter::Update (vtbl 0xAD)");
    }
}
