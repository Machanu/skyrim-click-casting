#include "ClickCast.h"

#include "Settings.h"

#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

// Animation-graph event sink: the cast lifecycle is event-driven now.
#include <atomic>
#include <cstring>

// ============================================================================
// Click-Casting core — single-hand latch / auto-release, plus symmetric dual-latch.
//
// Threading: the AttackBlockHandler::ProcessButton and
// PlayerCharacter::Update hooks both run on the game's main thread, so the latch state below is
// only ever touched from one thread — no synchronisation is required.
//
// The INPUT FIREWALL (ProcessInput_Hook) writes the other shared block, g_ep, and it is
// main-thread-serialised with those two BY CONSTRUCTION rather than by assumption:
//   * It hooks PlayerControls' BSTEventSink<InputEvent*>::ProcessEvent, which is the function
//     that walks the frame's event chain and DISPATCHES it to the PlayerInputHandlers. The
//     AttackBlockHandler::ProcessButton hook therefore executes INSIDE the original call this
//     hook makes at its end — same call stack, hence necessarily the same thread. The two can
//     never interleave: the firewall has finished all of its own writes before any handler runs.
//   * TickDualEpisode is called only from Update_Hook, i.e. the main thread, and input dispatch
//     and Actor::Update are separate phases of the same frame loop — never concurrent.
// So g_ep, g_phys, g_hand, g_drive, g_pending, g_deferred and the dwell counters stay plain
// (non-atomic) types, and that is a proof, not an assumption.
//
// The two SINKS are the exception, and both are already written for it: the animation-graph sink
// (see its own block below) and the MCM reload sink in Settings.cpp are delivered by
// BSTEventSource, which guards dispatch with a BSSpinLock without any header naming the thread.
// Neither touches the state above — the anim sink writes only std::atomic flags, and the settings
// sink writes only the atomics in Settings.cpp, which every hot-path read reaches through an
// atomic load.
//
// SETTINGS AND WORK IN FLIGHT: every MCM value is read at the point of use and
// never cached across a cast. Consequence, and it is the intended one: a toggle switched off
// mid-activity does NOT abort anything. EvaluateGate only ever runs BEFORE a latch, so a latch,
// a driving pair or a dual episode that is already under way completes under the rules it started
// with, and only NEW starts see the new setting. That is what preserves the file's central
// invariant — exactly one up per down reaches vanilla — across a settings change, and it is why
// nothing here force-resets state on reload.
//
// DUAL resolution, settled from in-game logs: a dual arrives as two SEPARATE Left+Right button
// events — the engine never emits a single "Dual Attack" event — and during the dual it drives
// ONLY ONE caster (empirically the LEFT) to kReady while the partner caster stays kNone. That is
// what the "driving-pair mode" block below exists for. The single-event DualEvent path is kept as
// a fallback in case another plugin does emit that control, but it never fires in vanilla.
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
    // holds the hand inert during the post-release firing window (the caster's kUnk04/kUnk08
    // phase — ~150 ms for a projectile, but 2-3 s for a self-target restoration spell) so a click
    // there cannot reach vanilla and cancel the just-fired cast. It is STATE-driven: it ends on
    // caster wind-down (kNone) or an equipped-object change. kReleasingSafetySecs is only an
    // emergency stop for a caster that never winds down, not the normal exit.
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
    //
    // This used to be `constexpr float kRecastDelaySecs = 0.25f`. It is now the
    // MCM slider Settings::RecastDelay(), whose CODE DEFAULT is that same 0.25 s and which is
    // clamped to [0.10, 1.00] — so an installation without ini files paces identically to v1.
    // Read at the point of use (arming a pending recast / arming the episode breather), never
    // cached, so a mid-session MCM change applies from the next cycle without disturbing a
    // countdown that is already running.

    // Pair stall guard. If the driving-pair driver sticks in its first charging state
    // (kUnk(1)) without progressing for this long, the engine has wedged: vanilla holds a down
    // that will never release. Abort the pair, clean up, and go Idle. a_delta-accumulated.
    constexpr float kPairStallSecs = 2.0f;

    // Releasing safety cap. Releasing is STATE-driven: it ends when the
    // driving caster winds down to kNone. This is only the absolute emergency stop for a caster
    // that never winds down at all. It used to be 1.5 s, which was shorter than the real post-fire
    // phase of self-target restoration spells (Close Wounds: kReady(3) -> kUnk04(4) -> kUnk08(8),
    // with kUnk08 running ~2.2-2.6 s), so a perfectly normal cast was cut short and the next latch
    // then fought the still-busy engine. Fireball goes 4 -> 0 in ~150 ms and never reaches this.
    // a_delta-accumulated, like every other threshold here.
    constexpr float kReleasingSafetySecs = 3.0f;

    // Recast wait cap. After the pacing delay elapses, the relatch waits
    // for the hand's caster to be fully back at kNone before any synthetic down goes out: starting
    // a cast while the engine is still finishing the previous one is what makes the engine abort
    // it half-way and leave the caster wedged in kUnk04. This is the upper bound on that wait —
    // past it the pending recast is dropped cleanly (no events delivered). a_delta-accumulated.
    constexpr float kRecastWaitCapSecs = 3.0f;

    // Wind-down failsafe. The cast lifecycle is driven by the engine's own
    // CastStop / InterruptCast animation events, but an event can in principle be missed (graph
    // rebuilt mid-cast, sink re-registered, unusual interrupt). This is the emergency valve: if
    // this long has passed since THAT HAND's fire delivery, its flag is dropped so the mod can
    // never be locked out permanently. Absolute elapsed time (a_delta-accumulated), deliberately
    // NOT conditioned on caster state — state flicker from vanilla charge/abort cycles used to
    // starve the old idle-based clock and deadlock it.
    constexpr float kWindDownFailsafeSecs = 4.0f;

    // Fire confirmation window. After a release we delivered, the engine is
    // expected to answer with that hand's MLh/MRh_SpellFire_Event (empirically ~0.37 s) or with
    // InterruptCast. If neither arrives within this long the engine never actually fired — the
    // exact signature of the old wedge — and the mod says so in one line. Detection only.
    constexpr float kFireConfirmSecs = 1.0f;

    // Physical hand-off grace. After a PHYSICAL up on a hand, one-click-dual
    // must not claim virtual ownership of that hand for this long: a hand between a spam-click's up
    // and its next down is not "free", it is mid-sequence, and a synthetic down dropped into that
    // gap collides with the incoming physical events. Short enough that a deliberate click on one
    // hand while the other rests still forms the dual immediately.
    constexpr float kPhysHandoffGraceSecs = 0.3f;

    // ---- ownership and settling rules ----------------------
    // minimum measured idle dwell before ANY latch entry point accepts a hand. The dwell has
    // been measured since the event model landed and printed on every latch line, but nothing
    // ever gated on it, so
    // an instantaneous kNone read (the engine's well-known mid-transition flicker) counted as
    // "settled". Calibrated from the logs: latches that produced a real cast sat at dwell >= 0.28 s,
    // the still-born ones at 0.00-0.01 s. 0.15 s separates the two with margin in both directions.
    // a_delta-accumulated like every other threshold here.
    constexpr float kSettleDwellSecs = 0.15f;

    // absolute cap on the settling window armed by a release we delivered that is NOT a fire
    // (cleanup balance, dissolve balance, hold-to-recast up). The engine still reacts to such a
    // release — a graph transition, not a cast run — so the hand must not count as settled while it
    // lands, but it must also not be blocked for the full fire failsafe. Deliberately equal to
    // the DEFAULT recast delay so the hold-to-recast tempo is unchanged (the two windows run
    // concurrently). It is only a CAP: CastStop / InterruptCast / the hand's own Equipped_Event
    // clear it earlier.: it stays a fixed 0.25 s and deliberately does NOT follow
    // the MCM slider — it bounds how long the ENGINE may take to settle, which is not a pacing
    // preference. Consequence at the bottom of the slider range: with the delay below 0.25 s this
    // window can outlast it and the relatch waits out the remainder. That is exactly the "very low
    // values can make the game pause briefly" note in the MCM help text — a stall guard, not a bug.
    constexpr float kSettleFailsafeSecs = 0.25f;

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
        float                   latchDwell = 0.0f;   // measured idle dwell when this latch was accepted
    };

    HandLatch g_hand[kHandCount];

    // ---- dual-pair coordination -------------------------------------------
    // Two ways a dual can arise, which this build's logging is designed to tell apart:
    //   * DualEvent : ONE physical "Dual Attack" button-down latched BOTH hands. New
    //     coordinated behaviour — wait until BOTH casters are kReady, then deliver a single
    //     synthetic "Dual Attack" release.
    //   * TwoEvent : independent Left + Right latches happened to be active at the same
    //     time. No new behaviour — each hand keeps its own single-hand release; the two
    //     virtual holds ARE the symmetric dual. The pair is only a log marker.
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

    // ---- driving-pair mode -------------------------
    // In-game diagnostics proved: a dual cast arrives as separate Left+Right events, and
    // during the dual the engine drives ONLY ONE caster (empirically the LEFT) to kReady while
    // the PARTNER caster sits at kNone the whole time. The old per-hand poll read that expected
    // partner-kNone as "charge interrupted" and reset the partner latch, so the partner's
    // physical button-up then reached vanilla and cancelled the dual charge.
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

    // Pair-veto log dedup. The veto in UpdateDrivePairActivation re-evaluates
    // EVERY frame, so one vetoed latch episode used to emit ~50 identical lines per second. The
    // line is now emitted once per episode: this flag is set when it is logged and cleared on
    // every frame the veto does NOT apply (pair formed, nothing latched, a hand Releasing, the
    // dual flag gone), so the next episode logs again.
    bool g_pairVetoLogged = false;

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

    // ---- physical-ownership guards -----------------
    // Seconds left of the hand-off grace after a PHYSICAL up on this hand. While it runs, the hand
    // is considered to be in the middle of physical activity even though no button is down, so
    // one-click-dual must not claim VIRTUAL ownership of it. A measured failure: the
    // trigger grabbed the left hand in the gap between a spam-click's up and the next down; the
    // synthetic left down then collided with the spam sequence, the driver's charge never started,
    // and the pair died in grace with an unowned down still held on the right.
    float g_physRecentUp[kHandCount] = { 0.0f, 0.0f };

    // Set when a delivered down was balanced for us while the button was STILL physically held
    // (pair-break balance). The next physical up on that hand is then swallowed, so vanilla never
    // receives two ups for one down.
    bool g_swallowNextUp[kHandCount] = { false, false };

    // A "player-owned input episode" tokenlived here. It was built to
    // stop one-click-dual from claiming a hand the player was operating; the dual-episode controller
    //replaced that whole family with its own demand model, and the token was left behind
    // with no reader — its only read decided when to clear itself. Removed together
    // with its ownership predicate. g_physRecentUp above is unrelated and still live.

    void TickPhysHandoffGrace(float a_delta)
    {
        for (std::size_t hand = 0; hand < kHandCount; ++hand) {
            if (g_physRecentUp[hand] > 0.0f) {
                g_physRecentUp[hand] -= a_delta;
                if (g_physRecentUp[hand] < 0.0f) {
                    g_physRecentUp[hand] = 0.0f;
                }
            }
        }
    }

    // ========================================================================
    // DUAL-EPISODE CONTROLLER
    //
    // Replaces the whole one-click-dual family (virtual ownership, the physical-wins dissolve,
    // the absorb branch, the same-spell deferred guard). Those were four separate owners of the
    // input stream, each holding it for a few frames, and every spam click landed in whichever
    // window happened to be open — which is why hold+spam never converged.
    //
    // In the SAME-SPELL configuration (the same fire-and-forget SPELL in both hands) ONE owner now
    // holds the whole stream: a firewall on PlayerControls' InputEvent sink removes the attack
    // events from the frame's chain before any handler sees them, and records nothing but DEMAND
    // ("the player wants this hand casting"). The controller then drives both hands together at
    // its own tempo, reusing the existing driving-pair engine unchanged as its motor.
    //
    // Ledger: the controller delivers a synthetic DOWN per hand per cycle; the balancing UP is
    // always produced by the machine that owns the latch afterwards — the pair's kReady double
    // release, or CleanupReleaseIfNeeded on any abnormal exit (a controller hand always has
    // g_phys.down == false, so cleanup is never skipped). Exactly one up per down, unchanged.
    struct DualEpisode
    {
        bool                    active = false;                    // the controller owns both hands
        bool                    demand[kHandCount] = { false, false };  // player wants this hand casting
        bool                    delivered[kHandCount] = { false, false };  // our down is outstanding
        RE::TESForm*            spell = nullptr;                   // the shared form this episode runs
        RE::INPUT_DEVICE        device = RE::INPUT_DEVICE::kKeyboard;  // payload for the synthetic downs
        std::uint32_t           idCode[kHandCount] = { 0, 0 };
        RE::PlayerControlsData* data = nullptr;
        float                   paceRemaining = 0.0f;              // breather between cast cycles
        bool                    ending = false;                    // demand gone, waiting out the run
        // Safety valve: a held button emits a continuation every frame, so silence means the up was
        // never seen (menu opened, another sink returned kStop, focus lost). Demand then drops.
        float                   silence[kHandCount] = { 0.0f, 0.0f };
        bool                    startBlockLogged = false;          // "start deferred" logged once
    };

    DualEpisode g_ep;

    // The controller is holding this hand down for vanilla right now.
    bool EpisodeHoldsHand(std::size_t a_hand) noexcept
    {
        return g_ep.active && g_ep.delivered[a_hand];
    }

    // "Held" for PAIR-VETO and RECAST-TEMPO purposes only: a real physical button, or a hand the
    // dual-episode controller is currently holding down. Never use this where a physical key
    // matters (cleanup, the recast up) — a controller hand has no physical key.
    bool HandHeld(std::size_t a_hand) noexcept
    {
        return g_phys[a_hand].down || EpisodeHoldsHand(a_hand);
    }

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
        // once the pacing delay has elapsed the recast additionally waits for
        // the caster to return to kNone. These track that second wait.
        float waitElapsed = 0.0f;   // seconds spent waiting for kNone after the delay elapsed
        bool  waitLogged = false;   // "recast wait" logged once per wait episode
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

    // One "[cc] busy" line per busy episode per hand (cleared on the next accepted latch).
    bool g_busyLogged[kHandCount] = { false, false };

    RE::MagicSystem::CastingSource CasterSource(std::size_t a_hand) noexcept
    {
        return a_hand == kLeft ? RE::MagicSystem::CastingSource::kLeftHand
                               : RE::MagicSystem::CastingSource::kRightHand;
    }

    RE::MagicCaster* GetCaster(RE::PlayerCharacter* a_pc, std::size_t a_hand)
    {
        return a_pc ? a_pc->GetMagicCaster(CasterSource(a_hand)) : nullptr;
    }

    // ---- idle dwell / settled gate -----------------
    // LOG-ONLY since the event model landed. Stable kNone was a decent proxy but still guessed at
    // what the engine was doing; the CastStop event answers it directly, so the dwell no longer gates
    // anything. It is kept purely as a measurement carried on the latch / fire / busy lines, so the
    // event-driven decisions can still be correlated against the old signal in the next test round.
    // Ticked once per frame at the top of Update_Hook.
    float g_idleDwell[kHandCount] = { 0.0f, 0.0f };

    void TickIdleDwell(RE::PlayerCharacter* a_pc, float a_delta)
    {
        for (std::size_t hand = 0; hand < kHandCount; ++hand) {
            auto* caster = GetCaster(a_pc, hand);
            if (caster && caster->state.get() == RE::MagicCaster::State::kNone) {
                g_idleDwell[hand] += a_delta;
            } else {
                g_idleDwell[hand] = 0.0f;  // any other state (or no caster) restarts the dwell
            }
        }
    }

    // ---- wind-down tracking ------------------------
    // The engine's own animation-graph events settled what no caster state or timer could. A
    // completed cast emits: [our release] -> M{L,R}h_SpellFire_Event (~0.37 s) -> CastStop
    // (~0.8 s) -> idle. The ONE failing cast in the anim-diag run was a relatch issued BEFORE the
    // previous CastStop; its fire was never followed by a SpellFire event (the engine never fired)
    // and the caster wedged. Rule: a new cast is safe exactly when the previous run's CastStop has
    // been seen.
    //
    // MODEL: STRICTLY PER-HAND blocking, with a global sweep on clear.
    //   * SET only by OUR OWN release delivery on THAT hand (NoteFireDelivered). A hand can never
    //     be blocked because the other hand fired — asynchronous independent casts (different
    //     spells, alternating clicks) therefore never wait on each other.
    //   * CLEARED for BOTH hands by the global CastStop / InterruptCast. CastStop carries no hand
    //     (only M{L,R}h_SpellFire_Event does), so attributing it is impossible; sweeping both is
    //     safe because clearing can only ever UNBLOCK, never block. Combined with the per-hand
    //     caster==kNone requirement, an over-eager clear cannot let a still-casting hand latch.
    //   * PAIR: the double release calls NoteFireDelivered for both hands, which is correct —
    //     both hands really did fire — and the single CastStop of that one engine run clears both.
    //     (A counting model would break here: two releases, one CastStop, count never reaches zero.)
    // It cannot deadlock: only our own delivery sets it, and CastStop, InterruptCast or the
    // per-hand failsafe always clears it.
    std::atomic<bool> g_windDownPending[kHandCount] = { { false }, { false } };

    // which ENGINE RUN armed each hand's flag. A global CastStop / InterruptCast belongs to
    // exactly ONE run — the oldest one still outstanding — so sweeping every hand let hand A's
    // CastStop clear hand B's flag while B's own run was still going. Ids are handed
    // out monotonically; the two hands of a driving pair deliberately SHARE one id, because that
    // really is one engine run with two releases (a counting model would break here). 0 = unarmed.
    std::atomic<std::uint64_t> g_windDownRun[kHandCount] = { { 0 }, { 0 } };
    std::atomic<std::uint64_t> g_runSeq{ 0 };

    // was this hand's flag armed by a real FIRE, or by a settling release (cleanup / dissolve
    // balance / recast up)? Only the failsafe cap differs — everything else treats them alike.
    // Main thread only (written next to the arming, read only by the failsafe tick).
    bool g_windDownIsFire[kHandCount] = { false, false };

    std::uint64_t NextRunId() noexcept
    {
        return g_runSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    // Fire confirmation (detection only — no gate reads these). The sink sets the hand's flag on
    // its SpellFire event; InterruptCast counts as an answer for both hands.
    std::atomic<bool> g_fireConfirmed[kHandCount] = { { true }, { true } };
    bool              g_fireWatchActive[kHandCount] = { false, false };   // main thread only
    float             g_fireWatchRemaining[kHandCount] = { 0.0f, 0.0f };  // main thread only

    // Seconds since THIS hand's fire delivery — the failsafe clock (main thread only).
    float g_windDownElapsed[kHandCount] = { 0.0f, 0.0f };
    bool  g_windDownFailsafeLogged[kHandCount] = { false, false };

    // THE shared gate predicate: the hand's caster is at kNone AND no cast run is still winding
    // down. Used by all three latch entry points (physical click, hold-to-recast, partner
    // force-latch). Only ever consulted BEFORE a latch — once latched, the polls drive on caster
    // state alone, so nothing here can stall a run that is already under way.
    bool WindDownPending(std::size_t a_hand) noexcept
    {
        return g_windDownPending[a_hand].load(std::memory_order_relaxed);
    }

    // an instantaneous kNone read is not "settled" — the engine dips through kNone while it is
    // still reacting to what we last delivered, and that flicker is what produced the still-born
    // dwell=0.00 latches. The dwell has been measured all along (TickIdleDwell) but no gate ever
    // read it; requiring a stable window here closes that for EVERY latch entry point at once:
    // physical click (hook), deferred latch, hold-to-recast, and partner force-latch.
    bool IsCasterSettled(std::size_t a_hand, RE::PlayerCharacter* a_pc)
    {
        auto* caster = GetCaster(a_pc, a_hand);
        return caster && caster->state.get() == RE::MagicCaster::State::kNone &&
               !WindDownPending(a_hand) &&
               g_idleDwell[a_hand] >= kSettleDwellSecs;
    }

    // ---- pair event mirroring ----------------------
    // Measured: during an ENGINE DUAL the graph emits ONLY the driver's per-hand
    // events. The partner hand produces no MLh/MRh_SpellFire_Event of its own (so the partner's
    // fire-confirmation watch always fired a false "fire not confirmed") and no Equipped_Event
    // either (so its wind-down could only be cleared by the global CastStop). While a pair run is
    // outstanding the DRIVER's per-hand events therefore speak for BOTH hands. Two INDEPENDENT
    // overlapping casts are NOT a pair and keep strict per-hand semantics — the mirror is armed
    // only by the driving-pair double release.
    std::atomic<bool>          g_pairMirror{ false };
    std::atomic<std::uint32_t> g_pairMirrorDriver{ 0 };

    bool MirrorsToPartner(std::size_t a_hand) noexcept
    {
        return g_pairMirror.load(std::memory_order_relaxed) &&
               g_pairMirrorDriver.load(std::memory_order_relaxed) == static_cast<std::uint32_t>(a_hand);
    }

    // Sink-side (possibly off the main thread): only atomics are touched.
    void NoteSpellFireEvent(std::size_t a_hand)
    {
        g_fireConfirmed[a_hand].store(true, std::memory_order_relaxed);
        if (MirrorsToPartner(a_hand)) {
            g_fireConfirmed[a_hand == kLeft ? kRight : kLeft].store(true, std::memory_order_relaxed);
        }
    }

    // The per-hand "this hand is safe again" mark. It arrives AFTER SpellFire and ~0.2 s BEFORE the
    // global CastStop, and in the measured wedge the failing relatch sat exactly before it — so it
    // is the correct release point, and SpellFire alone would be too early. Because it is per-hand,
    // a hand is never starved by the other hand holding the graph in a cast state (a held
    // concentration staff used to suppress CastStop for 11 s and lock the other hand out).
    void NoteHandEquippedEvent(std::size_t a_hand)
    {
        g_windDownPending[a_hand].store(false, std::memory_order_relaxed);
        g_windDownRun[a_hand].store(0, std::memory_order_relaxed);
        if (MirrorsToPartner(a_hand)) {
            const std::size_t other = (a_hand == kLeft) ? kRight : kLeft;
            g_windDownPending[other].store(false, std::memory_order_relaxed);
            g_windDownRun[other].store(0, std::memory_order_relaxed);
        }
    }

    // NOTE: an experimental "aggressive tempo" mode briefly let the per-hand
    // SpellFire event clear the wind-down block ~0.2 s earlier than the Equipped_Event does. It has
    // been removed again — SpellFire is measurably too early as a safety signal (it arrives before
    // the point the fix-6 wedge sat at), so the Equipped_Event below is the ONLY per-hand release
    // point, exactly as in v1.
    //
    // a global CastStop / InterruptCast is emitted by exactly ONE engine run — the oldest one
    // still outstanding. Clear only the hands that belong to that run; a hand armed by a LATER,
    // independent delivery keeps its flag. A driving pair shares one run id, so one CastStop still
    // clears both hands of the pair (the property this depends on). Sink-side: only atomics are
    // touched, and a lost race merely leaves a flag set for the failsafe to drop.
    void SweepOldestRunWindDown() noexcept
    {
        std::uint64_t oldest = 0;
        for (std::size_t hand = 0; hand < kHandCount; ++hand) {
            if (!g_windDownPending[hand].load(std::memory_order_relaxed)) {
                continue;
            }
            const std::uint64_t run = g_windDownRun[hand].load(std::memory_order_relaxed);
            if (oldest == 0 || run < oldest) {
                oldest = run;
            }
        }
        if (oldest == 0) {
            return;
        }
        for (std::size_t hand = 0; hand < kHandCount; ++hand) {
            if (g_windDownPending[hand].load(std::memory_order_relaxed) &&
                g_windDownRun[hand].load(std::memory_order_relaxed) == oldest) {
                g_windDownPending[hand].store(false, std::memory_order_relaxed);
                g_windDownRun[hand].store(0, std::memory_order_relaxed);
            }
        }
    }

    // Shared arming core. a_isFire selects the failsafe cap only.
    void ArmWindDown(std::size_t a_hand, std::uint64_t a_run, bool a_isFire)
    {
        g_windDownPending[a_hand].store(true, std::memory_order_relaxed);
        g_windDownRun[a_hand].store(a_run, std::memory_order_relaxed);
        g_windDownIsFire[a_hand] = a_isFire;
        g_windDownElapsed[a_hand] = 0.0f;
        g_windDownFailsafeLogged[a_hand] = false;
    }

    void ArmFireWatch(std::size_t a_hand)
    {
        g_fireConfirmed[a_hand].store(false, std::memory_order_relaxed);
        g_fireWatchActive[a_hand] = true;
        g_fireWatchRemaining[a_hand] = kFireConfirmSecs;
    }

    // We just delivered a release that can start a firing run on this hand. Arm the wind-down flag
    // (cleared by this hand's Equipped_Event, or by the global CastStop / InterruptCast sweep) and
    // the fire-confirmation watch.
    // any delivery that is NOT part of the driving pair's own double release ends the pair
    // mirror. The mirror must live exactly as long as the ONE engine run it was armed for —
    // letting it outlive that run let the old driver's Equipped_Event clear a NEW independent run's
    // wind-down on the partner hand. NoteFirePair re-arms it after its two calls.
    void NoteFireDelivered(std::size_t a_hand)
    {
        ArmWindDown(a_hand, NextRunId(), /*a_isFire=*/true);
        ArmFireWatch(a_hand);
        g_pairMirror.store(false, std::memory_order_relaxed);
    }

    // A driving-pair double release: one engine run, two hands, but only the DRIVER's per-hand
    // events will arrive. Arm both hands under ONE run id and point the mirror at the driver.
    void NoteFirePair(std::size_t a_driver, std::size_t a_other)
    {
        const std::uint64_t run = NextRunId();
        ArmWindDown(a_driver, run, /*a_isFire=*/true);
        ArmWindDown(a_other, run, /*a_isFire=*/true);
        ArmFireWatch(a_driver);
        ArmFireWatch(a_other);
        g_pairMirrorDriver.store(static_cast<std::uint32_t>(a_driver), std::memory_order_relaxed);
        g_pairMirror.store(true, std::memory_order_relaxed);
    }

    // EVERY up we hand to vanilla changes engine state, not just the fires. The cleanup balance
    // (CleanupReleaseIfNeeded), the pair-break balance (EndPair) and the
    // hold-to-recast up (DeliverRecastUp) used to arm nothing at all, which left IsCasterSettled
    // blind to the mod's OWN deliveries — the hole that let a deferred latch execute in the very
    // frame of a pair break.
    //
    // a_atReady: the hand's caster was at kReady when the release went out. Then the release is not
    // a balance at all — the engine FIRES — so it gets the full fire treatment (fire-confirmation
    // watch + the long failsafe cap). Otherwise only the short settling cap applies.
    // A settling arm never weakens a fire that is still outstanding.
    void NoteReleaseDelivered(std::size_t a_hand, bool a_atReady)
    {
        if (!a_atReady && WindDownPending(a_hand) && g_windDownIsFire[a_hand]) {
            return;  // a real fire is still winding down — keep its run id and its long cap
        }
        ArmWindDown(a_hand, NextRunId(), /*a_isFire=*/a_atReady);
        if (a_atReady) {
            ArmFireWatch(a_hand);
        }
        g_pairMirror.store(false, std::memory_order_relaxed);  //
    }

    // Did THIS hand's caster sit at kReady? Read BEFORE a release is delivered, so the caller can
    // tell a balance release apart from one the engine will turn into a real cast.
    bool CasterAtReady(std::size_t a_hand)
    {
        auto* caster = GetCaster(RE::PlayerCharacter::GetSingleton(), a_hand);
        return caster && caster->state.get() == RE::MagicCaster::State::kReady;
    }

    // Emergency valve, on ABSOLUTE time since this hand's fire. The old version only
    // counted time in which both casters were idle, which deadlocked under click spam: the
    // repeated vanilla charge/abort cycles kept flipping the caster state, so the idle clock never
    // ran. Elapsed-since-fire cannot be starved by state flicker.
    void TickWindDownFailsafe(float a_delta)
    {
        for (std::size_t hand = 0; hand < kHandCount; ++hand) {
            if (!WindDownPending(hand)) {
                g_windDownElapsed[hand] = 0.0f;
                g_windDownFailsafeLogged[hand] = false;
                continue;
            }
            // a settling arm (cleanup / dissolve balance / recast up) is capped much shorter
            // than a fire — the engine only has a graph transition to land, not a cast run to
            // finish, and the hand must not be held out of play for the fire failsafe.
            const float cap = g_windDownIsFire[hand] ? kWindDownFailsafeSecs : kSettleFailsafeSecs;
            g_windDownElapsed[hand] += a_delta;
            if (g_windDownElapsed[hand] > cap) {
                if (g_windDownIsFire[hand] && !g_windDownFailsafeLogged[hand]) {
                    g_windDownFailsafeLogged[hand] = true;
                    spdlog::info("[cc] windDown failsafe {} (no CastStop {:.1f}s after fire)",
                        HandName(hand), kWindDownFailsafeSecs);
                }
                // A settling window expiring is the NORMAL close of a balance release — no line.
                g_windDownPending[hand].store(false, std::memory_order_relaxed);
                g_windDownRun[hand].store(0, std::memory_order_relaxed);
                g_windDownElapsed[hand] = 0.0f;
            }
        }

        // The pair mirror lives exactly as long as the pair run it was armed for: once neither hand
        // is winding down any more (Equipped_Event, CastStop, InterruptCast or the failsafe above),
        // the next fire starts from strict per-hand semantics again.
        if (!WindDownPending(kLeft) && !WindDownPending(kRight)) {
            g_pairMirror.store(false, std::memory_order_relaxed);
        }
    }

    // ---- deferred latch ----------------------------
    // A physical DOWN that arrives while this hand is still winding down must NOT reach vanilla.
    // Letting it through (the old busy pass-through) started a self-reinforcing loop: vanilla began
    // and immediately aborted a charge for every click (BeginCastRight every ~180 ms for 8.5 s in
    // the 23:22 log), the constant aborts kept the animation graph in its cast state, CastStop was
    // pushed further out, wind-down stayed set, more clicks were passed through — and so on.
    //
    // Instead the event is SWALLOWED and the intent is remembered: the hand keeps the click as a
    // deferred latch and performs it automatically the moment the wind-down clears and the caster
    // is back at kNone. The equipped form is snapshotted at click time; if the equip changed in
    // between, the deferred latch is dropped cleanly instead of casting something else.
    //
    // EVENT BALANCE — vanilla sees NOTHING for a deferred episode until it executes:
    //   * executed: the down was swallowed, so PerformRecast delivers a fresh synthetic DOWN and
    //     the hand enters the ordinary latch lifecycle — from there the normal release / cleanup /
    //     hold-to-recast rules own the balance exactly as for any other latch. One down, one up.
    //   * cancelled (button released) / dropped (equip changed, hand no longer Idle): NOTHING was
    //     ever delivered — no down, so no up is owed, and the physical UP is swallowed too. The
    //     ledger is untouched on both sides.
    struct DeferredLatch
    {
        bool         active = false;
        RE::TESForm* spell = nullptr;  // equipped form snapshotted at click time
        // the same-spell hold-off logs once per waiting episode.
        bool         holdLogged = false;
    };

    DeferredLatch g_deferred[kHandCount];

    void ClearDeferredLatch(std::size_t a_hand) noexcept
    {
        g_deferred[a_hand].active = false;
        g_deferred[a_hand].spell = nullptr;
        g_deferred[a_hand].holdLogged = false;
    }

    // One "[cc] deferred latch" line per episode; further clicks while it waits are silent.
    // a_reason names the entry route (wind-down, virtual dissolve, same-spell partner).
    void ArmDeferredLatch(std::size_t a_hand, RE::TESForm* a_spell, const char* a_reason)
    {
        if (g_deferred[a_hand].active) {
            return;  // already waiting — swallow the repeat click without a line
        }
        g_deferred[a_hand].active = true;
        g_deferred[a_hand].spell = a_spell;
        g_deferred[a_hand].holdLogged = false;
        spdlog::info("[cc] deferred latch {} '{}' ({} -> waiting)",
            HandName(a_hand), a_spell ? a_spell->GetName() : "?", a_reason);
    }

    void CancelDeferredLatch(std::size_t a_hand, const char* a_reason)
    {
        if (!g_deferred[a_hand].active) {
            return;
        }
        spdlog::info("[cc] deferred latch {} cancelled ({})", HandName(a_hand), a_reason);
        ClearDeferredLatch(a_hand);
    }

    // One line if the engine never answered our fire. Detection only — nothing branches on it.
    void TickFireConfirm(float a_delta)
    {
        for (std::size_t hand = 0; hand < kHandCount; ++hand) {
            if (!g_fireWatchActive[hand]) {
                continue;
            }
            if (g_fireConfirmed[hand].load(std::memory_order_relaxed)) {
                g_fireWatchActive[hand] = false;
                continue;
            }
            g_fireWatchRemaining[hand] -= a_delta;
            if (g_fireWatchRemaining[hand] <= 0.0f) {
                g_fireWatchActive[hand] = false;
                spdlog::info("[cc] fire not confirmed {}", HandName(hand));
            }
        }
    }

    // ========================================================================
    // Animation-graph event sink — the engine's authoritative cast lifecycle
    //
    // MagicCaster::state flickers (kNone dips mid-aftermath) and timers only approximate it, so
    // the machine runs on the engine's own animation events instead. A diagnostic run
    // measured a completed cast as: [our release] ->
    // M{L,R}h_SpellFire_Event (~0.37 s) -> CastStop (~0.8 s) -> idle. CastStop / InterruptCast
    // clear the wind-down flag; the per-hand SpellFire events answer the fire-confirmation watch.
    // Only VANILLA tags are used — State Behavior Framework's SBF_* events are deliberately
    // ignored, so the plugin works without it.
    //
    // The "[cc-anim]" whitelist logging (IsCastAnimTag / kAnimCastTags) is kept as diagnostics for
    // bug reports: it is what makes a "my spell did not fire" log readable.
    //
    // Header-verified (vcpkg CommonLibSSE-NG 3.7.0 include tree):
    //   RE/B/BSAnimationGraphEvent.h:9-17  struct BSAnimationGraphEvent { const BSFixedString tag;   // 00
    //                                        const TESObjectREFR* holder; const BSFixedString payload; } == 0x18
    //   RE/B/BSTEvent.h:142-147            template<class Event> class BSTEventSink { virtual BSEventNotifyControl
    //                                        ProcessEvent(const Event*, BSTEventSource<Event>*) = 0; }
    //   RE/B/BSTEvent.h:11-13              enum class BSEventNotifyControl { kContinue = 0, ... }
    //   RE/A/Actor.h:494                   bool AddAnimationGraphEventSink(BSTEventSink<BSAnimationGraphEvent>*) const;
    //   RE/A/Actor.h:603                   void RemoveAnimationGraphEventSink(BSTEventSink<BSAnimationGraphEvent>*) const;
    //   RE/B/BSTEvent.h:34-56              AddEventSink() de-duplicates (std::find before push_back) and defers
    //                                        while notifying -> repeated registration is idempotent and safe.
    //
    // Threading: BSTEventSource guards AddEventSink / RemoveEventSink / SendEvent with a BSSpinLock
    // (RE/B/BSTEvent.h:40, :70, :91), i.e. the engine expects concurrent access, but NO header says
    // which thread delivers the events. The sink is therefore written thread-safe regardless: it
    // touches nothing but std::atomic<bool> flags (g_windDownPending, g_fireConfirmed) and spdlog
    // (main.cpp:39 builds a basic_file_sink_mt, which is thread-safe). It never reads or writes
    // latch, pair, pending or dwell state — the main thread alone consumes the flags.
    constexpr float kAnimRegisterRetrySecs = 5.0f;

    // Cast-related tags logged unconditionally (CK wiki + existing SKSE mods). Compared
    // case-insensitively because behaviour-file tag casing varies between mod authors.
    const char* const kAnimCastTags[] = {
        "BeginCastLeft", "BeginCastRight", "BeginCastVoice",
        "CastStop", "CastOKStart", "CastOKStop",
        "InterruptCast",
        "MLh_SpellFire_Event", "MRh_SpellFire_Event", "Voice_SpellFire_Event",
        "MLh_SpellRelease_Event", "MRh_SpellRelease_Event",
        "MagicCastRelease", "MagicCastLoopStart", "MagicCastLoopEnd",
        "SpellFire", "SpellRelease",
        "MLh_Equipped_Event", "MRh_Equipped_Event",   // the per-hand "hand is safe again" mark
    };

    bool IsCastAnimTag(const char* a_tag) noexcept
    {
        if (!a_tag) {
            return false;
        }
        for (const char* known : kAnimCastTags) {
            if (_stricmp(a_tag, known) == 0) {
                return true;
            }
        }
        return false;
    }

    // ---- BOW anim tags -----------------------------------
    // Vanilla 1hm_behavior event names, verified against the game's own behaviour-graph string
    // table.
    // Case varies between behaviour authors, hence the same _stricmp matching the cast tags use.
    //
    // STRICT ISOLATION: these tags NEVER reach the spell dispatch chain above (SweepOldestRunWindDown,
    // NoteSpellFireEvent, NoteHandEquippedEvent) and the spell tags never reach the bow one. The two
    // vocabularies are disjoint by name, and the sink branches on the bow list only after the whole
    // spell chain has been evaluated and did not match.
    const char* const kAnimBowTags[] = {
        "bowDrawStart", "bowDrawn",
        "arrowRelease", "BowRelease",
        "attackStop", "bowEnd", "bowReset", "arrowDetach",
    };

    bool IsBowAnimTag(const char* a_tag) noexcept
    {
        if (!a_tag) {
            return false;
        }
        for (const char* known : kAnimBowTags) {
            if (_stricmp(a_tag, known) == 0) {
                return true;
            }
        }
        return false;
    }

    // Set by the sink when the engine says the bow cycle is over (bowEnd / bowReset / arrowDetach).
    // Sink-side, so it is an atomic and NOTHING else is touched from the sink; the main thread's
    // TickBowEpisode consumes it. It is only a BACKSTOP for a poll that missed the transition —
    // the attack-state poll remains the primary signal.
    std::atomic<bool> g_bowAnimEnd{ false };

    // Raised by the MAIN thread while a bow is equipped with the gate open, so the sink can keep the
    // "[cc-bow-anim]" lines low-frequency without ever reading game state off-thread. "attackStop"
    // in particular is emitted by every melee swing too; without this gate it would flood the log.
    std::atomic<bool> g_bowLogArmed{ false };

    void NoteBowAnimEvent(const char* a_tag) noexcept
    {
        if (_stricmp(a_tag, "bowEnd") == 0 || _stricmp(a_tag, "bowReset") == 0 ||
            _stricmp(a_tag, "arrowDetach") == 0) {
            g_bowAnimEnd.store(true, std::memory_order_relaxed);
        }
    }

    struct AnimEventSink : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
    {
        RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent*                  a_event,
                                              RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
        {
            const char* tag = a_event ? a_event->tag.c_str() : nullptr;
            if (!tag) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // ---- the signals the machine actually runs on (vanilla tags only) ----
            // Deliberately NOT State Behavior Framework's SBF_* events: the plugin works without it.
            // CastStop / InterruptCast carry no hand. Since they no longer sweep BOTH blindly:
            // they clear the OLDEST outstanding run only (SweepOldestRunWindDown), which still
            // covers a driving pair (one shared run id) but can no longer close a newer,
            // independent run on the other hand.
            if (_stricmp(tag, "CastStop") == 0) {
                SweepOldestRunWindDown();   // only the run this CastStop belongs to
            } else if (_stricmp(tag, "InterruptCast") == 0) {
                SweepOldestRunWindDown();   //
                g_fireConfirmed[kLeft].store(true, std::memory_order_relaxed);
                g_fireConfirmed[kRight].store(true, std::memory_order_relaxed);
            } else if (_stricmp(tag, "MLh_SpellFire_Event") == 0) {
                NoteSpellFireEvent(kLeft);
            } else if (_stricmp(tag, "MRh_SpellFire_Event") == 0) {
                NoteSpellFireEvent(kRight);
            } else if (_stricmp(tag, "MLh_Equipped_Event") == 0) {
                NoteHandEquippedEvent(kLeft);    // per-hand release point
            } else if (_stricmp(tag, "MRh_Equipped_Event") == 0) {
                NoteHandEquippedEvent(kRight);
            } else if (IsBowAnimTag(tag)) {
                // Reached ONLY when no spell tag matched, so the bow branch can
                // never disturb the run-id sweep, the wind-down flags or the dual-episode state.
                // It writes exactly one atomic, which TickBowEpisode consumes on the main thread.
                NoteBowAnimEvent(tag);
                if (g_bowLogArmed.load(std::memory_order_relaxed)) {
                    spdlog::info("[cc-bow-anim] {}", tag);
                }
            }

            // ---- [cc-anim] diagnostics for bug reports ----
            if (IsCastAnimTag(tag)) {
                const char* payload = a_event->payload.c_str();
                spdlog::info("[cc-anim] {} (payload='{}')", tag, payload ? payload : "");
            }
            return RE::BSEventNotifyControl::kContinue;  // never consume — pure observer
        }
    };

    AnimEventSink g_animSink;

    // Frame-driven (re)registration. AddEventSink de-duplicates, so re-arming every few seconds is
    // free and transparently covers a player graph rebuilt by a load-game, new-game or race change
    // without needing a second SKSE messaging listener. Logged only when the result changes.
    float g_animRegisterTimer = 0.0f;
    bool  g_animRegisterOk = false;

    void TickAnimSinkRegistration(float a_delta)
    {
        g_animRegisterTimer -= a_delta;
        if (g_animRegisterTimer > 0.0f) {
            return;
        }
        g_animRegisterTimer = kAnimRegisterRetrySecs;

        auto*      pc = RE::PlayerCharacter::GetSingleton();
        const bool ok = pc && pc->AddAnimationGraphEventSink(&g_animSink);
        if (ok != g_animRegisterOk) {
            g_animRegisterOk = ok;
            spdlog::info("[cc-anim] sink registration -> {}", ok ? "ok" : "failed (player graph not ready)");
        }
    }
    // ======================= end [cc-anim] block ============================

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
        L.latchDwell = 0.0f;

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

    // ---- two-handed signature -------------------
    // Partner-driver adoption was designed for forms that are equipped in BOTH hands and run ONE
    // cast on ONE caster — two-handed scrolls and ritual/master spells. The old trigger only tested
    // "same form pointer + partner caster active", which an ordinary one-hand spell held in both
    // hands satisfies just as well during the engine's dual-decision window, so adoption fired on
    // normal duals and hijacked them. Gate it on the real two-handed signature instead.
    //
    // Detection (CommonLibSSE-NG 3.7.0 headers, verified):
    //   RE/M/MagicItem.h:100   virtual bool IsTwoHanded() const;   // vtbl 0x67, base { return false; }
    //   RE/S/SpellItem.h:82    bool IsTwoHanded() const override;  // 0x67 - { BGSEquipSlot* equipSlot =
    //                            BGSEquipType::GetEquipSlot(); return equipSlot && equipSlot->flags & 1; }
    //   RE/B/BGSEquipType.h:30/34  GetEquipSlot() / BGSEquipSlot* equipSlot;  // ETYP
    //   RE/B/BGSEquipSlot.h:19     Flag::kUseAllParents = 1 << 0   <- the bit tested above
    //   RE/S/ScrollItem.h:12-13    class ScrollItem : public SpellItem  (inherits the same override)
    // IsTwoHanded() IS the engine's own BothHands test: the BothHands equip slot is the one whose
    // DATA flags carry bit 0. BGSDefaultObjectManager only exposes kLeftHandEquip(19) /
    // kRightHandEquip(20) / kEitherHandEquip(21) — it has NO BothHands entry — so this vfunc is the
    // only unambiguous route, and it is a public const method on SpellItem.
    bool IsTwoHandedCastable(const RE::TESForm* a_form)
    {
        if (!a_form) {
            return false;
        }
        const RE::FormType ft = a_form->GetFormType();
        if (ft == RE::FormType::Scroll) {
            return true;  // a scroll auto-equips into both hands
        }
        if (ft != RE::FormType::Spell) {
            return false;  // staves and everything else are never adopted
        }
        return static_cast<const RE::SpellItem*>(a_form)->IsTwoHanded();  // BothHands equip slot
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

            // per-type MCM toggle. A disabled type is a plain gate FAIL, i.e. the
            // ordinary pass-through every non-castable already takes — so that type behaves exactly
            // like vanilla, and no other branch of the machine is aware the toggle exists. Logged
            // through LogPassThrough, so it stays deduped to one line per equipped form per hand.
            const bool typeEnabled = (ft == RE::FormType::Spell) ? Settings::SpellCasting()
                                                                 : Settings::ScrollCasting();
            if (!typeEnabled) {
                LogPassThrough(a_hand, form, spell->GetName(),
                    ft == RE::FormType::Spell ? "spell casting off (MCM)" : "scroll casting off (MCM)");
                return nullptr;
            }

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

        // ---- Staff: Weapon + IsStaff + FaF enchantment + chargeTime>0.
        // Empirically a FaF staff drives the hand caster through the SAME kUnk->kReady
        // sequence as a spell, so the machine is reused 1:1 with the staff weapon as L.spell.
        if (ft == RE::FormType::Weapon) {
            auto* weap = static_cast<RE::TESObjectWEAP*>(form);

            // ---- Bow ------------------------------------------------------
            // A bow is driven by its OWN state machine (see the "[cc-bow]" block below), which
            // polls Actor attack state rather than MagicCaster state. EvaluateGate therefore
            // deliberately keeps returning nullptr for it: it is the spell machine's gate, and a
            // weapon must never be able to enter a spell latch, a driving pair, a deferred latch or
            // a hold-to-recast. The bow branch in ProcessButton_Hook runs BEFORE this gate is ever
            // consulted, so reaching here with a bow simply means the bow machine declined the
            // event and vanilla should have it untouched.
            //
            // Only bows. Every other weapon type falls through to the "not a staff" pass-through
            // below and behaves exactly as it did before this machine existed.
            //
            // TESObjectWEAP::IsBow() — RE/T/TESObjectWEAP.h:252 (WEAPON_TYPE kBow = 7, :53).
            if (weap->IsBow()) {
                LogPassThrough(a_hand, form, weap->GetName(), "bow: bow machine owns it");
                return nullptr;
            }

            if (!weap->IsStaff()) {
                LogPassThrough(a_hand, form, weap->GetName(), "not a staff");
                return nullptr;
            }
            // staff toggle. Checked AFTER IsStaff() so a non-staff weapon keeps
            // its own, more informative pass-through reason.
            if (!Settings::StaffCasting()) {
                LogPassThrough(a_hand, form, weap->GetName(), "staff casting off (MCM)");
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

    // ========================================================================
    // BOW AUTO-SHOT
    //
    // A SEPARATE, self-contained state machine. It shares exactly three things with the spell
    // machine — the ProcessButton hook it is dispatched from, the Update hook that ticks it, and
    // the ButtonEvent::Create -> g_originalProcessButton delivery mechanism — and NOTHING else. It
    // never touches g_hand, g_pair, g_drive, g_ep, g_pending, g_deferred, g_phys, the wind-down
    // flags or the run-id sweep, and no spell path reads any bow state. That isolation is the whole
    // reason a bow can be added without re-testing the spell side.
    //
    // WHY attack state and not a caster: a bow has no MagicCaster. The engine's own draw state is
    // Actor's ATTACK_STATE_ENUM, reached through Actor::AsActorState() because this is an NG
    // multi-target build (ENABLE_SKYRIM_SE/AE/VR are all defined, so RE/A/Actor.h:128-130 takes the
    // `#else` branch and Actor inherits ONLY TESObjectREFR — pc->GetAttackState() does not exist).
    //   RE/A/Actor.h:724-730       Actor::AsActorState()  -> RelocateMemberIfNewer(1.6.629, 0xB8, 0xC0)
    //   RE/A/ActorState.h:155      ActorState::GetAttackState() { return actorState1.meleeAttackState; }
    //   RE/A/ActorState.h:20-39    ATTACK_STATE_ENUM: kBowDraw=8, kBowAttached=9, kBowDrawn=10,
    //                              kBowReleasing=11, kBowReleased=12, kBowNextAttack=13,
    //                              kBowFollowThrough=14
    // kBowDrawn (10) is the full-draw point; independently corroborated by Bow PowerShot's
    // own OAR condition ("AttackState" == 10.0 together with IsEquippedType 7).
    //
    // THE LEDGER, which is the same invariant the spell side keeps: vanilla receives exactly one up
    // for every down it received. The bow machine only ever SWALLOWS ups and only ever INJECTS ups,
    // one-for-one, and every exit path (fire, abort, timeout, anim-backstop, equip change) delivers
    // the owed up before clearing. Downs are ALWAYS passed through untouched.

    // Full draw must hold for this many consecutive polled frames before the synthetic release goes
    // out. One frame is not enough: the engine flickers through states on transitions exactly as it
    // does for the magic caster (the reason kSettleDwellSecs exists on the spell side).
    constexpr int kBowDrawnDwellFrames = 2;

    // Same dwell, applied to the abort test, so a single-frame dip to kNone mid-transition does not
    // tear down a perfectly healthy draw.
    constexpr int kBowAbortDwellFrames = 2;

    // ---- Bow Charge Plus delay (opt-in) --------------------
    // The auto path releases ~13-14 ms after kBowDrawn, which is far inside Bow Charge Plus's own
    // charge window: BCP counts its first level from full draw, and manual releases were measured
    // catching level 1 at down to 433 ms (measured 2026-08). Holding the drawn bow this much
    // longer lets that level register.
    //
    // The first value tried was 500 ms — the measured 433 ms plus what looked like enough margin.
    // It was not: 512 ms of DELIVERED hold caught level 1 in only ~80% of shots. BCP polls from
    // Papyrus, so the moment it samples the draw drifts from shot to shot, and clearing the
    // threshold on average is not the same as clearing it every time. 650 ms covers that jitter
    // with room to spare, at a cost the player has explicitly opted into.
    //
    // Deliberately a FIXED constant and not a slider: it exists to clear one specific, measured
    // threshold in one specific mod, and a value below it would silently do nothing at all while
    // still costing the player the delay. The MCM exposes only whether it applies.
    constexpr float kBcpDelaySecs = 0.65f;

    // Absolute cap on a pending release. Past this the engine never reached full draw and never
    // will (menu, stagger, load, a behaviour mod that took the graph elsewhere), so the owed up is
    // delivered and the episode is dropped. a_delta-accumulated, like every threshold in this file.
    constexpr float kBowPendingTimeoutSecs = 10.0f;

    enum class BowPhase
    {
        Idle,
        ReleasePending  // we swallowed a physical up and owe vanilla a synthetic one
    };

    struct BowEpisode
    {
        BowPhase                phase = BowPhase::Idle;
        std::size_t             owner = kHandCount;  // control track that claimed this button cycle
        bool                    physDown = false;    // owner's physical button is down right now
        // Payload of the owner's physical down, replayed as the synthetic up.
        RE::INPUT_DEVICE        device = RE::INPUT_DEVICE::kKeyboard;
        RE::BSFixedString       userEvent;
        std::uint32_t           idCode = 0;
        RE::PlayerControlsData* data = nullptr;
        // Poll bookkeeping.
        float                   pendingElapsed = 0.0f;
        int                     drawnFrames = 0;
        int                     abortFrames = 0;
        std::uint32_t           lastState = 0;       // for the transition lines
        // "gate open" is emitted the moment the engine actually STARTS A DRAW, not when the
        // episode is claimed — a bow bash claims a cycle exactly like a draw click does, and used
        // to leave a misleading gate-open line behind. The facts the line reports are captured at
        // claim time and held here until there is something worth announcing.
        bool                    gateLogged = false;  // the episode has been announced
        std::int32_t            gateAmmo = 0;
        bool                    gateLeftSlot = false;
        // accumulated hold time for the synthetic held-event stream. Seeded from the
        // engine's own heldDownSecs on the swallowed physical up, then advanced by a_delta once
        // per frame. This IS the value the final release carries, so the draw reads as a full one.
        float                   heldSecs = 0.0f;
        bool                    heldStreamLogged = false;
        // the opt-in Bow Charge Plus extension. bcpElapsed only advances once the full-draw
        // dwell has been satisfied; bcpUsed is logging-only, so the fire line can say whether the
        // extension actually ran.
        float                   bcpElapsed = 0.0f;
        bool                    bcpUsed = false;
    };

    BowEpisode g_bow;

    // The bow machine delivered the owed up while the button was STILL physically held (a spam
    // click, or a hold that outlived the shot). Vanilla is therefore holding nothing, and the
    // eventual physical up must be swallowed so it cannot become a second release for one down.
    // Exactly the role g_swallowNextUp plays on the spell side; kept separate so the two ledgers
    // can never interfere.
    bool g_bowSwallowNextUp[kHandCount] = { false, false };

    const char* BowStateName(std::uint32_t a_state) noexcept
    {
        using S = RE::ATTACK_STATE_ENUM;
        switch (static_cast<S>(a_state)) {
        case S::kNone:             return "kNone";
        case S::kDraw:             return "kDraw";
        case S::kSwing:            return "kSwing";
        case S::kHit:              return "kHit";
        case S::kNextAttack:       return "kNextAttack";
        case S::kFollowThrough:    return "kFollowThrough";
        case S::kBash:             return "kBash";
        case S::kBowDraw:          return "kBowDraw";
        case S::kBowAttached:      return "kBowAttached";
        case S::kBowDrawn:         return "kBowDrawn";
        case S::kBowReleasing:     return "kBowReleasing";
        case S::kBowReleased:      return "kBowReleased";
        case S::kBowNextAttack:    return "kBowNextAttack";
        case S::kBowFollowThrough: return "kBowFollowThrough";
        default:                   return "kUnk";
        }
    }

    std::uint32_t BowAttackState(RE::PlayerCharacter* a_pc) noexcept
    {
        if (!a_pc) {
            return 0;
        }
        return static_cast<std::uint32_t>(a_pc->AsActorState()->GetAttackState());
    }

    // The draw is genuinely in flight: the engine has started nocking/pulling but has not reached
    // full draw yet. This — not "anything below kBowDrawn" — is what makes a swallowed up safe,
    // because it guarantees something IS coming that can reach kBowDrawn. See the note above.
    bool BowDrawInFlight(std::uint32_t a_state) noexcept
    {
        return a_state == static_cast<std::uint32_t>(RE::ATTACK_STATE_ENUM::kBowDraw) ||
               a_state == static_cast<std::uint32_t>(RE::ATTACK_STATE_ENUM::kBowAttached);
    }

    // The shot has already left, one way or another — nothing more to wait for.
    bool BowShotGone(std::uint32_t a_state) noexcept
    {
        using S = RE::ATTACK_STATE_ENUM;
        const auto s = static_cast<S>(a_state);
        return s == S::kBowReleasing || s == S::kBowReleased ||
               s == S::kBowNextAttack || s == S::kBowFollowThrough;
    }

    // The equipped BOW, whichever hand slot the engine reports it in. The headers could not settle from the
    // headers which slot a two-handed bow occupies (RE/A/Actor.h:538 declares
    // GetEquippedObject(bool a_leftHand) with no implementation to read), so both are tried rather
    // than assumed. a_outLeftSlot reports which one answered, purely for the diagnostics.
    RE::TESObjectWEAP* EquippedBow(RE::PlayerCharacter* a_pc, bool* a_outLeftSlot = nullptr)
    {
        if (!a_pc) {
            return nullptr;
        }
        for (const bool leftSlot : { false, true }) {
            RE::TESForm* form = a_pc->GetEquippedObject(leftSlot);
            if (form && form->GetFormType() == RE::FormType::Weapon) {
                auto* weap = static_cast<RE::TESObjectWEAP*>(form);
                if (weap->IsBow()) {
                    if (a_outLeftSlot) {
                        *a_outLeftSlot = leftSlot;
                    }
                    return weap;
                }
            }
        }
        return nullptr;
    }

    // Equipped ammo count. GetCurrentAmmo is Actor's virtual 0x9E (RE/A/Actor.h:344); the count
    // comes from PlayerCharacter::GetItemCount (RE/P/PlayerCharacter.h:558), which takes a
    // TESBoundObject* — TESAmmo derives from it.
    std::int32_t BowAmmoCount(RE::PlayerCharacter* a_pc, RE::TESAmmo** a_outAmmo = nullptr)
    {
        if (!a_pc) {
            return 0;
        }
        RE::TESAmmo* ammo = a_pc->GetCurrentAmmo();
        if (a_outAmmo) {
            *a_outAmmo = ammo;
        }
        if (!ammo) {
            return 0;
        }
        return a_pc->GetItemCount(ammo);
    }

    // ---- synthetic button delivery ------------------------------------------
    // Identical mechanism to the spell side's DeliverRecastUp: build a ButtonEvent and hand it
    // straight to the ORIGINAL ProcessButton, so it cannot re-enter our own hook and cannot be seen
    // by the input firewall (which sits on PlayerControls' InputEvent chain, a different function
    // entirely). The event is ours — created, not taken from the queue — so we destroy it after.
    //
    // Classification is purely (value, heldDownSecs), verified in RE/B/ButtonEvent.h:18-24:
    //   IsDown() = value > 0 && heldDownSecs == 0
    //   IsHeld() = value > 0 && heldDownSecs >  0
    //   IsUp()   = value == 0 && heldDownSecs >  0
    // so a HELD frame is value 1.0 with a rising heldDownSecs, and the release is value 0.0
    // carrying the SAME accumulated total.
    bool DeliverBowSyntheticButton(float a_value, float a_heldSecs, const char* a_tag)
    {
        auto* controls = RE::PlayerControls::GetSingleton();
        RE::AttackBlockHandler* handler = controls ? controls->attackBlockHandler : nullptr;
        if (!handler || !g_bow.data || g_originalProcessButton.address() == 0) {
            spdlog::error("[cc-bow] ERROR {}: cannot deliver synthetic button (handler={}, data={}, orig={})",
                a_tag,
                static_cast<const void*>(handler),
                static_cast<const void*>(g_bow.data),
                (g_originalProcessButton.address() != 0 ? "ok" : "null"));
            return false;
        }
        RE::ButtonEvent* ev =
            RE::ButtonEvent::Create(g_bow.device, g_bow.userEvent, g_bow.idCode, a_value, a_heldSecs);
        if (!ev) {
            spdlog::error("[cc-bow] ERROR {}: ButtonEvent::Create returned null", a_tag);
            return false;
        }
        g_originalProcessButton(handler, ev, g_bow.data);
        ev->~ButtonEvent();
        RE::free(ev);
        return true;
    }

    // One held frame of the virtual hold. Neither a down nor an up, so it moves NO ledger anywhere:
    // g_bowSwallowNextUp and g_phys are only ever written from the isDown / isUp branches of our own
    // hook, and this never passes through that hook.
    void DeliverBowSyntheticHeld(float a_heldSecs)
    {
        DeliverBowSyntheticButton(1.0f, a_heldSecs, "held");
    }

    // The release. heldDownSecs carries the FULL accumulated hold, which is what an earlier build got
    // wrong: it sent a fixed 0.1 s, so the engine saw a 0.1 s draw and the arrow left with no force.
    void DeliverBowSyntheticUp(const char* a_tag)
    {
        // IsUp() requires heldDownSecs > 0 (ButtonEvent.h:24) — never let a zero through, or the
        // event would classify as neither an up nor a held and be silently inert.
        const float held = g_bow.heldSecs > 0.0f ? g_bow.heldSecs : 0.1f;
        if (!DeliverBowSyntheticButton(0.0f, held, a_tag)) {
            return;
        }

        // We just balanced a down that the player is still physically holding, so the physical up
        // that eventually arrives is already spent. Swallow it.
        if (g_bow.physDown && g_bow.owner < kHandCount) {
            g_bowSwallowNextUp[g_bow.owner] = true;
        }
    }

    void BowClearEpisode() noexcept
    {
        g_bow.phase = BowPhase::Idle;
        g_bow.owner = kHandCount;
        g_bow.physDown = false;
        g_bow.data = nullptr;
        g_bow.idCode = 0;
        g_bow.userEvent = RE::BSFixedString();
        g_bow.device = RE::INPUT_DEVICE::kKeyboard;
        g_bow.pendingElapsed = 0.0f;
        g_bow.drawnFrames = 0;
        g_bow.abortFrames = 0;
        g_bow.lastState = 0;
        g_bow.gateLogged = false;
        g_bow.gateAmmo = 0;
        g_bow.gateLeftSlot = false;
        g_bow.heldSecs = 0.0f;
        g_bow.heldStreamLogged = false;
        g_bow.bcpElapsed = 0.0f;
        g_bow.bcpUsed = false;
        g_bowAnimEnd.store(false, std::memory_order_relaxed);
    }

    // Settle an outstanding pending release and drop the episode. Used by every abnormal exit;
    // the owed up ALWAYS goes out first, so the ledger balances on every path.
    void BowAbortPending(std::uint32_t a_state, const char* a_reason)
    {
        if (g_bow.phase == BowPhase::ReleasePending) {
            spdlog::info("[cc-bow] aborted draw cleanup (state={}({}) reason={})",
                a_state, BowStateName(a_state), a_reason);
            DeliverBowSyntheticUp("abort");
        }
        BowClearEpisode();
    }

    // Announce the episode — once, and only once the engine has actually started a DRAW. A bow
    // BASH claims a cycle exactly like a draw click does (same control tracks, same gate), so
    // announcing at claim time filled the log with gate-open lines for bashes that never drew
    // anything. Deferring the line costs nothing: everything it reports is captured at claim.
    // This is LOGGING ONLY — no decision anywhere reads gateLogged.
    void MaybeLogBowGateOpen(RE::PlayerCharacter* a_pc, std::uint32_t a_state)
    {
        if (g_bow.gateLogged || g_bow.owner >= kHandCount) {
            return;
        }
        g_bow.gateLogged = true;
        RE::TESObjectWEAP* weap = EquippedBow(a_pc);
        spdlog::info("[cc-bow] gate open hand={} weapon='{}' ammo={} (slot={}, state={}({}))",
            g_bow.owner == kLeft ? "L" : "R", weap ? weap->GetName() : "?", g_bow.gateAmmo,
            g_bow.gateLeftSlot ? "left" : "right", a_state, BowStateName(a_state));
    }

    // ---- button dispatch ----------------------------------------------------
    // What the hook should do with this event. NotBow means "the bow machine has no opinion" and
    // the caller continues into the unchanged spell path.
    enum class BowAction
    {
        NotBow,
        PassThrough,
        Swallow
    };

    BowAction ProcessBowButton(std::size_t a_hand, RE::ButtonEvent* a_event,
                               RE::PlayerControlsData* a_data, bool a_isDown, bool a_isUp)
    {
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) {
            return BowAction::NotBow;
        }

        RE::TESObjectWEAP* weap = EquippedBow(pc);
        if (!weap) {
            // Not a bow. If an episode was somehow still open (swap mid-draw), settle it.
            if (g_bow.owner != kHandCount) {
                BowAbortPending(BowAttackState(pc), "bow unequipped");
            }
            return BowAction::NotBow;
        }

        if (!Settings::BowAutoShot()) {
            if (g_bow.owner != kHandCount) {
                BowAbortPending(BowAttackState(pc), "bBowAutoShot turned off");
            }
            return BowAction::NotBow;  // toggle off == vanilla, exactly like every other type
        }

        RE::TESAmmo*       ammo  = nullptr;
        const std::int32_t count = BowAmmoCount(pc, &ammo);
        if (!ammo || count <= 0) {
            if (g_bow.owner != kHandCount) {
                BowAbortPending(BowAttackState(pc), "out of ammo");
            }
            return BowAction::NotBow;  // no ammo: nothing can be drawn, stay out of the way
        }

        // A hand that is mid-latch on the SPELL machine keeps its own path. This cannot normally
        // happen with a bow equipped (PollOnce resets on equip change), but the bow machine must
        // never step over a latch that still owes vanilla an up.
        if (g_hand[a_hand].phase != Phase::Idle) {
            return BowAction::NotBow;
        }

        const std::uint32_t state = BowAttackState(pc);

        // A previously injected release already balanced this hand's held down — swallow the
        // physical up that pays for it. A fresh DOWN below starts a new ledger and clears the flag.
        if (a_isUp && g_bowSwallowNextUp[a_hand]) {
            g_bowSwallowNextUp[a_hand] = false;
            if (g_bow.owner == a_hand) {
                g_bow.physDown = false;
            }
            return BowAction::Swallow;
        }

        // ---- ownership: one control track drives one physical button cycle ----
        // Whichever track sees the DOWN first owns the cycle; the partner track's events for that
        // same cycle are passed through untouched and never latch. Ownership is released as soon as
        // the owner's cycle ends, so the next DOWN — from either track — can claim freshly. The
        // diagnostics below record which track actually drives a draw.
        if (a_isDown) {
            const bool canClaim = (g_bow.phase == BowPhase::Idle) &&
                                  (g_bow.owner == kHandCount || !g_bow.physDown);
            g_bowSwallowNextUp[a_hand] = false;  // a fresh press starts a new ledger
            if (canClaim) {
                g_bow.owner     = a_hand;
                g_bow.physDown  = true;
                g_bow.device    = a_event->device.get();
                g_bow.userEvent = a_event->QUserEvent();
                g_bow.idCode    = a_event->GetIDCode();
                g_bow.data      = a_data;
                g_bow.lastState = state;
                // capture what the gate saw, but do not announce it yet — the line is emitted
                // by MaybeLogBowGateOpen once a draw actually starts, so a bash stays silent.
                bool leftSlot = false;
                EquippedBow(pc, &leftSlot);
                g_bow.gateAmmo     = count;
                g_bow.gateLeftSlot = leftSlot;
            } else if (g_bow.owner == a_hand) {
                // a fresh down during a pending release is passed through and must NEVER
                // cancel the pending release or reset the phase. Only the physical flag moves.
                g_bow.physDown = true;
            }
            return BowAction::PassThrough;  // the draw always starts naturally
        }

        if (a_isUp) {
            if (g_bow.owner != a_hand) {
                return BowAction::PassThrough;  // partner track — untouched, no latch
            }
            g_bow.physDown = false;

            // while a release is already pending we owe vanilla exactly ONE up
            // and it is ours to deliver. A spam-click's up is therefore swallowed too — passing it
            // through would be a second release for one held down.
            if (g_bow.phase == BowPhase::ReleasePending) {
                return BowAction::Swallow;
            }

            const bool inFlight = BowDrawInFlight(state);
            // a very fast click can release before the poll ever saw the draw, so announce the
            // episode here too. Either way the gate-open line precedes the decision line, and a
            // bash — which never reaches a draw state — produces neither.
            if (inFlight) {
                MaybeLogBowGateOpen(pc, state);
            }
            if (g_bow.gateLogged) {
                spdlog::info("[cc-bow] up-decision state={}({}) -> {}",
                    state, BowStateName(state), inFlight ? "auto" : "manual");
            }

            if (inFlight) {
                // CLICK. Swallow the up and hold it until full draw.
                //
                // seed the virtual hold from the ENGINE's own accumulated value on this
                // very up (ButtonEvent::heldDownSecs, RE/B/ButtonEvent.h:28 — the engine has been
                // filling it since the physical down). From here TickBowEpisode keeps it rising,
                // so the hold never appears to stall just because the physical stream stopped.
                g_bow.phase          = BowPhase::ReleasePending;
                g_bow.pendingElapsed = 0.0f;
                g_bow.drawnFrames    = 0;
                g_bow.abortFrames    = 0;
                g_bow.lastState      = state;
                g_bow.heldSecs       = a_event->heldDownSecs;
                g_bowAnimEnd.store(false, std::memory_order_relaxed);
                return BowAction::Swallow;
            }

            // HOLD (already at or past full draw), or no draw in flight at all. Pure
            // pass-through — no swallow, no synthetic event, nothing owed. This is the path that
            // keeps Bow Charge Plus / Bow PowerShot / bow bash working exactly as they do today.
            BowClearEpisode();
            return BowAction::PassThrough;
        }

        // ---- held continuation ----
        // while a release is pending, WE are the only source of held frames. Swallowing
        // the physical ones keeps a single, monotonically rising heldDownSecs — otherwise a
        // spam-click's fresh physical stream (which restarts its own count at ~0) would fight the
        // virtual hold and the engine would see the draw collapse and restart.
        if (g_bow.owner == a_hand && g_bow.phase == BowPhase::ReleasePending) {
            return BowAction::Swallow;
        }
        // Not pending: vanilla owns the stream and must keep drawing. Track the engine's own value
        // so a later swallow can seed from it even if the up itself arrives malformed.
        if (g_bow.owner == a_hand && a_event->heldDownSecs > g_bow.heldSecs) {
            g_bow.heldSecs = a_event->heldDownSecs;
        }
        return BowAction::PassThrough;
    }

    // ---- per-frame poll -----------------------------------------------------
    void TickBowEpisode(RE::PlayerCharacter* a_pc, float a_delta)
    {
        if (!a_pc) {
            return;
        }

        RE::TESObjectWEAP* weap = EquippedBow(a_pc);

        // Arm the sink's low-frequency bow logging only while a bow is actually equipped.
        g_bowLogArmed.store(weap != nullptr, std::memory_order_relaxed);

        if (g_bow.owner == kHandCount) {
            return;  // no episode claimed
        }

        // The bow went away (swap, sheathe, unequip) — settle whatever is owed.
        if (!weap) {
            BowAbortPending(BowAttackState(a_pc), "bow no longer equipped");
            return;
        }

        const std::uint32_t state = BowAttackState(a_pc);

        // announce the episode the first time the engine is genuinely drawing. Everything the
        // line reports was captured at claim; deferring it keeps a bow BASH — which claims a cycle
        // the same way but never enters a draw state — completely out of the log.
        if (BowDrawInFlight(state) ||
            state == static_cast<std::uint32_t>(RE::ATTACK_STATE_ENUM::kBowDrawn)) {
            MaybeLogBowGateOpen(a_pc, state);
        }

        // Transitions are part of the same announcement: silent until the episode is announced, so
        // a bash leaves no orphaned state lines behind either. lastState still tracks throughout.
        if (state != g_bow.lastState) {
            if (g_bow.gateLogged) {
                spdlog::info("[cc-bow] state {}({}) -> {}({})",
                    g_bow.lastState, BowStateName(g_bow.lastState), state, BowStateName(state));
            }
            g_bow.lastState = state;
        }

        if (g_bow.phase != BowPhase::ReleasePending) {
            // Idle episode: it only exists to hold ownership while the button is down. Once the
            // button is up and the engine is back to rest, release the claim.
            if (!g_bow.physDown && state == static_cast<std::uint32_t>(RE::ATTACK_STATE_ENUM::kNone)) {
                BowClearEpisode();
            }
            return;
        }

        g_bow.pendingElapsed += a_delta;

        // ---- THE VIRTUAL HOLD -----------------------------------------------------------------
        // The physical up was swallowed, so the input layer stops emitting held frames for this
        // control and the handler's own accumulation freezes — the animation kept playing (it runs
        // off the behaviour graph), which is why an earlier build looked correct in the log while
        // the arrow left with no force. One synthetic held frame per rendered frame replaces it:
        // value 1.0 with a heldDownSecs that keeps rising from where the engine left off.
        g_bow.heldSecs += a_delta;
        if (!g_bow.heldStreamLogged) {
            g_bow.heldStreamLogged = true;
            spdlog::info("[cc-bow] held-stream start (from={:.2f}s)", g_bow.heldSecs);
        }
        DeliverBowSyntheticHeld(g_bow.heldSecs);

        // (1) Full draw reached — the normal, expected exit.
        if (state == static_cast<std::uint32_t>(RE::ATTACK_STATE_ENUM::kBowDrawn)) {
            g_bow.abortFrames = 0;
            if (++g_bow.drawnFrames >= kBowDrawnDwellFrames) {
                // opt-in Bow Charge Plus extension. The held stream above keeps running while
                // this counts down, so the engine sees one continuous, still-rising hold — the draw
                // is simply held longer before it is let go. Read at the point of use like every
                // other setting, so toggling mid-draw affects the NEXT shot, never this one.
                // With the toggle OFF nothing below executes and the fire is unchanged.
                if (Settings::BowChargePlusDelay()) {
                    g_bow.bcpElapsed += a_delta;
                    if (g_bow.bcpElapsed < kBcpDelaySecs) {
                        g_bow.bcpUsed = true;
                        return;  // keep holding — the abort tests below still guard every exit
                    }
                }
                if (g_bow.bcpUsed) {
                    spdlog::info("[cc-bow] synthetic release injected (state={}({}), dwell={} frames, "
                                 "t={:.2f}, heldDownSecs={:.2f}, bcpDelay={:.2f})",
                        state, BowStateName(state), g_bow.drawnFrames, g_bow.pendingElapsed,
                        g_bow.heldSecs, g_bow.bcpElapsed);
                } else {
                    spdlog::info("[cc-bow] synthetic release injected (state={}({}), dwell={} frames, "
                                 "t={:.2f}, heldDownSecs={:.2f})",
                        state, BowStateName(state), g_bow.drawnFrames, g_bow.pendingElapsed,
                        g_bow.heldSecs);
                }
                DeliverBowSyntheticUp("fire");
                BowClearEpisode();
            }
            return;
        }
        // Left full draw — the dwell restarts, and so does the extension: a draw that dipped out of
        // kBowDrawn and came back has to earn the hold again.
        g_bow.drawnFrames = 0;
        g_bow.bcpElapsed = 0.0f;

        // (2) The shot already left without us (a behaviour mod, a perk, a competing plugin). The
        // owed up still has to go out, or vanilla keeps holding the draw forever.
        if (BowShotGone(state)) {
            BowAbortPending(state, "shot already released elsewhere");
            return;
        }

        // (3) Anim backstop: the engine says the bow cycle ended. Only ever a safety net
        // for a poll that missed the transition — it can only ever end an episode, never start one.
        if (g_bowAnimEnd.load(std::memory_order_relaxed)) {
            BowAbortPending(state, "anim end (bowEnd/bowReset/arrowDetach)");
            return;
        }

        // (4) Draw collapsed back to rest — stagger, menu, weapon swap, interrupted pull.
        if (state == static_cast<std::uint32_t>(RE::ATTACK_STATE_ENUM::kNone)) {
            if (++g_bow.abortFrames >= kBowAbortDwellFrames) {
                BowAbortPending(state, "draw collapsed to kNone");
            }
            return;
        }
        g_bow.abortFrames = 0;

        // (5) Absolute timeout.
        if (g_bow.pendingElapsed > kBowPendingTimeoutSecs) {
            spdlog::warn("[cc-bow] pending release timed out after {:.1f}s (state={}({}))",
                g_bow.pendingElapsed, state, BowStateName(state));
            BowAbortPending(state, "timeout");
        }
    }

    // ---- dual-episode configuration detection --------------------
    // The controller's entire domain: the SAME fire-and-forget SPELL in both hands. Identity is
    // pointer equality on the two gate results — the same convention the whole file uses — and it
    // is restricted to FormType::Spell, so scrolls (auto-equipped in both hands) and staves keep
    // every one of their existing paths. Returns the shared form, or nullptr.
    //
    // Note: EvaluateGate maintains the per-hand pass-through log dedup (g_ptLogged); calling it
    // here can therefore consume a dedup slot. That is pre-existing behaviour for every caller.
    RE::TESForm* SameSpellBothHands(RE::PlayerCharacter* a_pc)
    {
        if (!a_pc) {
            return nullptr;
        }
        RE::TESForm* left = EvaluateGate(a_pc, kLeft);
        if (!left || left->GetFormType() != RE::FormType::Spell) {
            return nullptr;
        }
        return (EvaluateGate(a_pc, kRight) == left) ? left : nullptr;
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
    // and, if the button is now physically UP, is left holding it forever (hanging hold).
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
    // a_evenIfHeld: balance the hand EVEN IF its button is still physically
    // down. The "skip while held" shortcut assumes the hand keeps owning its own release — true
    // while the latch lives, but NOT when the machine is about to go Idle with a delivered down
    // still outstanding. That is exactly what stranded a hand in an endless charge:
    // the pair broke in grace, both hands went Idle, and nothing ever released the right hand.
    // Pair exits therefore force the balance and swallow the eventual physical up instead.
    void CleanupReleaseIfNeeded(std::size_t a_hand, const char* a_reason, bool a_evenIfHeld = false)
    {
        const bool held = g_phys[a_hand].down;
        if (held && !a_evenIfHeld) {
            spdlog::info("[cc] cleanup {} skipped (btn still held) ({})", HandName(a_hand), a_reason);
            return;
        }
        if (!g_hand[a_hand].data) {
            return;  // nothing captured to balance against
        }
        if (held) {
            // WE own this hand's down (the physical one was swallowed by the latch). Balance it now
            // and swallow the eventual physical up, so vanilla never sees two ups for one down.
            g_swallowNextUp[a_hand] = true;
            spdlog::info("[cc] cleanup {} (btn still held, balancing delivered down) ({})",
                HandName(a_hand), a_reason);
        } else {
            spdlog::info("[cc] cleanup {} (btn up, no release delivered) ({})", HandName(a_hand), a_reason);
        }
        // (D1): read the caster BEFORE delivering — at kReady this "balance" is a real fire.
        const bool atReady = CasterAtReady(a_hand);
        SynthesizeRelease(a_hand);
        NoteReleaseDelivered(a_hand, atReady);
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
    // kReady. Every decision is logged with [recast]. The hand must be Idle, and the caller
    // (TickPendingRecasts / RecastGate::Ready) guarantees the hand's caster is at kNone — both
    // hands of a pair are checked BEFORE either down is delivered, so the guarantee cannot be
    // invalidated by the partner's own down inside the same frame.
    // A "which route asked for this relatch" enum lived here. It stopped selecting behaviour
    // when the one-click-dual trigger was removed, and the parameter was discarded on entry.
    // Removed; a_transition already names the route in every log line.
    bool PerformRecast(std::size_t a_hand, RE::PlayerCharacter* a_pc, const char* a_transition)
    {
        if (g_hand[a_hand].phase != Phase::Idle) {
            spdlog::info("[cc] recast {} skip (no longer Idle) [{}]", HandName(a_hand), a_transition);
            return false;
        }
        auto& P = g_phys[a_hand];
        // Hold-to-recast replays the tracked PHYSICAL button. A dual-episode hand never reaches
        // here: the controller owns its tempo and no pending recast is ever armed for it.
        if (!P.down) {
            spdlog::info("[cc] recast {} skip (btn not held) [{}]", HandName(a_hand), a_transition);
            return false;
        }
        const RE::INPUT_DEVICE  rcDevice = P.device;
        const std::uint32_t     rcIdCode = P.idCode;
        RE::PlayerControlsData* rcData   = P.data;
        if (rcDevice == RE::INPUT_DEVICE::kGamepad) {
            spdlog::info("[cc] recast {} skip (gamepad)", HandName(a_hand));
            return false;
        }
        if (!a_pc || !rcData) {
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
        L.device = rcDevice;
        L.userEvent = userEvents ? (a_hand == kLeft ? userEvents->leftAttack : userEvents->rightAttack)
                                 : RE::BSFixedString();
        L.idCode = rcIdCode;
        L.data = rcData;
        L.lastState = caster ? caster->state.get() : RE::MagicCaster::State::kNone;
        L.releasingElapsed = 0.0f;
        L.dualEventHand = false;
        L.lastDualCasting = caster ? caster->GetIsDualCasting() : false;
        L.chargeStarted = false;
        L.graceElapsed = 0.0f;
        L.graceLogged = false;
        L.latchElapsed = 0.0f;
        L.chargeTime = chargeTime;
        L.latchDwell = g_idleDwell[a_hand];   // dwell that satisfied RecastGate::Ready

        // The relatch itself is implicit — the preceding "recast ... armed" line and the following
        // "fire" line bracket it, so a hold cycle stays at two lines.

        // Feed vanilla a fresh down so it begins charging (a click's pass-through equivalent).
        DeliverSyntheticDown(a_hand);

        return true;
    }

    // Schedule a pending recast at a Releasing->Idle transition: if the button is still held,
    // arm the 0.25 s delay; the actual relatch happens on expiry (PerformRecast). If the button
    // is not held, there is nothing to recast.
    // a_deliverUp: false when the caller has ALREADY balanced this hand's delivered down (the
    // pair-break cleanup release). Vanilla is then holding nothing, so an up here would be a second
    // release for one down — arm the delay only, and let PerformRecast deliver the next fresh down.
    void SchedulePendingRecast(std::size_t a_hand, const char* a_transition, bool a_deliverUp = true)
    {
        const bool physHeld = g_phys[a_hand].down;
        if (!physHeld) {
            g_pending[a_hand].active = false;
            g_pending[a_hand].waitElapsed = 0.0f;
            g_pending[a_hand].waitLogged = false;
            return;  // plain click, or a controller hand: nothing to recast here — no line
        }
        // Button still held: the firing is done, but vanilla will auto-recharge because the key
        // is physically down. Deliver ONE synthetic UP to stop that, then arm the delay — the mod
        // now owns the recast tempo. The physical up is swallowed while pending (no double up).
        if (a_deliverUp) {
            // (D3): the up itself changes engine state (it stops vanilla's auto-recharge), so it
            // arms the short settling window too. It is never a fire — this hand has just finished
            // one — and NoteReleaseDelivered leaves that fire's longer window alone if it is still
            // outstanding, so the hold tempo is unchanged.
            DeliverRecastUp(a_hand);
            NoteReleaseDelivered(a_hand, /*a_atReady=*/false);
        }
        const float recastDelay = Settings::RecastDelay();   // MCM slider, default 0.25 s
        g_pending[a_hand].active = true;
        g_pending[a_hand].remaining = recastDelay;
        g_pending[a_hand].waitElapsed = 0.0f;
        g_pending[a_hand].waitLogged = false;
        spdlog::info("[cc] recast {} armed(+{:.2f}s)", HandName(a_hand), recastDelay);
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
                NoteFireDelivered(other);
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
        // FIX 2: but NOT when the partner runs the same-form cast — a release on
        // this control would cancel that running cast. Suppress and log.
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
        // Gate: ONLY a genuinely two-handed form may be adopted. L.spell is
        // the partner's form here — PartnerActiveSameForm requires pointer equality — so testing it
        // is testing the partner's equipped form. An ordinary one-hand spell in both hands can no
        // longer be adopted, which also stops adoption from hijacking the one-click-dual window.
        if (L.phase == Phase::Latched && !L.adopted && !L.dualEventHand && !L.chargeStarted &&
            state == RE::MagicCaster::State::kNone && IsTwoHandedCastable(L.spell) &&
            PartnerActiveSameForm(a_hand, pc)) {
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
            if (L.releasingElapsed > kReleasingSafetySecs) {
                spdlog::info("[cc] safety {} Releasing timeout @{:.1f}s state={}({}) -> Idle",
                    HandName(a_hand), kReleasingSafetySecs,
                    static_cast<std::uint32_t>(driveState), StateName(driveState));
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
                    NoteFireDelivered(kLeft);
                    NoteFireDelivered(kRight);
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
                    NoteFireDelivered(a_hand);
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
            // dwell= is the idle dwell measured when THIS latch was accepted — for a hold-to-recast
            // relatch (which emits no latch line of its own) this is where it is reported.
            spdlog::info("[cc] fire {} t={:.2f} dwell={:.2f} (kReady->release{})",
                HandName(a_hand), L.latchElapsed, L.latchDwell, L.adopted ? ", adopted driver" : "");
            SynthesizeRelease(a_hand);  // release the LATCHED hand (whose button was clicked)
            NoteFireDelivered(a_hand);
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
        // force the balance even for a physically-held hand. The pair is going Idle, so no
        // one would own that delivered down afterwards — that is what left a hand charging forever.
        if (!g_drive.released) {
            for (const std::size_t h : { kLeft, kRight }) {
                if (g_hand[h].phase == Phase::Latched) {
                    CleanupReleaseIfNeeded(h, a_reason, /*a_evenIfHeld=*/true);
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
        // a pair that breaks BEFORE its release leaves every hand Idle with its delivered down
        // already balanced above — including a hand whose button the player is STILL holding. That
        // hand used to be inert until it was released and pressed again ("the hand stops dead",
        // the audit), because nothing schedules a relatch on this path. Arm the ordinary
        // hold-to-recast delay for it instead. a_deliverUp=false: the cleanup release above already
        // balanced the down, so vanilla holds nothing and an up here would be a second release.
        const bool breakBeforeRelease = !g_drive.released;

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

        // last, so both hands are already Idle. A dual-episode hand has no physical button,
        // so it is skipped here: the controller owns its tempo and re-delivers on its own schedule.
        if (breakBeforeRelease) {
            for (const std::size_t h : { kLeft, kRight }) {
                if (g_phys[h].down) {
                    SchedulePendingRecast(h, "pair break (btn still held)", /*a_deliverUp=*/false);
                }
            }
        }
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

        ///3: never force-latch a hand that is not SETTLED — the synthetic down
        // would land in a post-fire phase (or a momentary kNone dip) and get the cast aborted.
        // Both callers already treat false as "no pair / no one-click dual", and during a REAL
        // engine dual the partner caster has been sitting at kNone the whole time (that is the
        // driving-pair premise), so its dwell is large and this rejects only the genuinely busy case.
        if (!IsCasterSettled(a_hand, a_pc)) {
            return false;
        }

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
        L.latchDwell = g_idleDwell[a_hand];
        // Force-latching the partner is part of forming the pair — the "[cc] pair" line reports it.
        return true;
    }

    // ========================================================================
    // DUAL-EPISODE CONTROLLER — the engine
    //
    // Runs once per frame from Update_Hook, immediately BEFORE UpdateDrivePairActivation, so a
    // cycle's two synthetic downs are in place before the pair can form in the very same frame —
    // the same discipline the deferred/pending ticks already follow.
    //
    // The controller never polls a caster itself. It only decides WHEN to deliver a pair of downs;
    // everything after that (pair formation, driver selection, grace, stall, the kReady double
    // release, NoteFirePair/windDown/pairMirror/run-id) is the existing driving-pair engine,
    // unchanged.
    constexpr float kDemandSilenceSecs = 0.35f;   // no event for this long -> the up was lost

    void EpisodeLogDemand(std::size_t a_hand, bool a_want)
    {
        spdlog::info("[cc-ep] demand {} {}", HandName(a_hand), a_want ? "on" : "off");
    }

    // Latch one hand from the episode payload and feed vanilla its down. Mirrors PerformRecast's
    // field setup exactly; the difference is only where the payload comes from.
    bool EpisodeDeliverHand(std::size_t a_hand, RE::PlayerCharacter* a_pc)
    {
        float        chargeTime = 0.0f;
        RE::TESForm* spell      = EvaluateGate(a_pc, a_hand, &chargeTime);
        if (spell != g_ep.spell || !g_ep.data) {
            return false;
        }
        auto* userEvents = RE::UserEvents::GetSingleton();
        auto* caster     = GetCaster(a_pc, a_hand);
        auto& L          = g_hand[a_hand];

        L.phase = Phase::Latched;
        L.spell = spell;
        L.device = g_ep.device;
        L.userEvent = userEvents ? (a_hand == kLeft ? userEvents->leftAttack : userEvents->rightAttack)
                                 : RE::BSFixedString();
        L.idCode = g_ep.idCode[a_hand];
        L.data = g_ep.data;
        L.lastState = caster ? caster->state.get() : RE::MagicCaster::State::kNone;
        L.releasingElapsed = 0.0f;
        L.dualEventHand = false;
        L.lastDualCasting = caster ? caster->GetIsDualCasting() : false;
        L.chargeStarted = false;
        L.graceElapsed = 0.0f;
        L.graceLogged = false;
        L.adopted = false;
        L.latchElapsed = 0.0f;
        L.chargeTime = chargeTime;
        L.latchDwell = g_idleDwell[a_hand];

        DeliverSyntheticDown(a_hand);
        g_ep.delivered[a_hand] = true;
        return true;
    }

    void EpisodeEnd(const char* a_reason)
    {
        if (!g_ep.active) {
            return;
        }
        spdlog::info("[cc-ep] end ({})", a_reason);
        g_ep.active = false;
        g_ep.ending = false;
        g_ep.spell = nullptr;
        g_ep.data = nullptr;
        g_ep.paceRemaining = 0.0f;
        g_ep.startBlockLogged = false;
        for (const std::size_t h : { kLeft, kRight }) {
            g_ep.delivered[h] = false;
        }
    }

    void TickDualEpisode(RE::PlayerCharacter* a_pc, float a_delta)
    {
        // there is no separate dual-cast toggle any more — dual casting is
        // simply what Spell Casting does. The controller's own gate is therefore only the configuration
        // test (SameSpellBothHands below), which runs through EvaluateGate and already honours
        // bSpellCasting: with spells disabled it returns nullptr, no episode ever starts, and a
        // physical L+R dual behaves exactly as it does without this machine.

        // ---- demand upkeep: the safety valve for an up we never saw ----
        for (const std::size_t h : { kLeft, kRight }) {
            if (!g_ep.demand[h]) {
                g_ep.silence[h] = 0.0f;
                continue;
            }
            g_ep.silence[h] += a_delta;
            if (g_ep.silence[h] > kDemandSilenceSecs) {
                g_ep.demand[h] = false;
                g_ep.silence[h] = 0.0f;
                spdlog::info("[cc-ep] demand {} off (no input {:.2f}s)", HandName(h), kDemandSilenceSecs);
            }
        }
        const bool anyDemand = g_ep.demand[kLeft] || g_ep.demand[kRight];

        // A hand still carries our delivered down until it returns to Idle — the machine that owns
        // the latch balances it there (the pair's kReady double release, or CleanupReleaseIfNeeded
        // on any abnormal exit; a controller hand always has g_phys.down == false, so cleanup is
        // never skipped). Nothing is owed once the hand is Idle again.
        for (const std::size_t h : { kLeft, kRight }) {
            if (g_ep.delivered[h] && g_hand[h].phase == Phase::Idle && !g_drive.active) {
                g_ep.delivered[h] = false;
            }
        }
        const bool busy = g_drive.active || g_ep.delivered[kLeft] || g_ep.delivered[kRight] ||
                          g_hand[kLeft].phase != Phase::Idle || g_hand[kRight].phase != Phase::Idle;

        RE::TESForm* config = SameSpellBothHands(a_pc);

        // ---- episode teardown ----
        if (g_ep.active) {
            if (config != g_ep.spell) {
                g_ep.ending = true;   // equip swap / unequip / sheathe
            }
            if (!anyDemand) {
                g_ep.ending = true;
            }
            if (g_ep.ending && !busy) {
                EpisodeEnd(config != g_ep.spell ? "equip changed" : "demand released");
                return;
            }
            if (g_ep.ending) {
                return;  // never interrupt a run — wait it out, the machine balances it
            }
        }

        // ---- episode start ----
        if (!g_ep.active) {
            if (!anyDemand || !config) {
                g_ep.startBlockLogged = false;
                return;
            }
            const bool clear = !busy && !g_deferred[kLeft].active && !g_deferred[kRight].active &&
                               !g_pending[kLeft].active && !g_pending[kRight].active &&
                               IsCasterSettled(kLeft, a_pc) && IsCasterSettled(kRight, a_pc);
            if (!clear) {
                if (!g_ep.startBlockLogged) {
                    g_ep.startBlockLogged = true;
                    spdlog::info("[cc-ep] start deferred (hands not clear: phase L={} R={}, wd L={} R={})",
                        static_cast<int>(g_hand[kLeft].phase), static_cast<int>(g_hand[kRight].phase),
                        WindDownPending(kLeft) ? 1 : 0, WindDownPending(kRight) ? 1 : 0);
                }
                return;
            }
            g_ep.active = true;
            g_ep.ending = false;
            g_ep.spell = config;
            g_ep.paceRemaining = 0.0f;
            g_ep.startBlockLogged = false;
            // The controller now owns both hands. Nothing else may believe a button is held for
            // them: the firewall eats every attack event from here on, so any g_phys left over
            // from before the configuration held would never be cleared by an up.
            for (const std::size_t h : { kLeft, kRight }) {
                g_phys[h].down = false;
                g_swallowNextUp[h] = false;
                g_pending[h].active = false;
                ClearDeferredLatch(h);
            }
            spdlog::info("[cc-ep] start spell='{}' (demand L={} R={})",
                config->GetName(), g_ep.demand[kLeft] ? 1 : 0, g_ep.demand[kRight] ? 1 : 0);
        }

        // ---- cast cycle ----
        if (busy) {
            g_ep.paceRemaining = Settings::RecastDelay();   // breather starts when the run lets go
            return;
        }
        if (g_ep.paceRemaining > 0.0f) {
            g_ep.paceRemaining -= a_delta;
            return;
        }
        if (!IsCasterSettled(kLeft, a_pc) || !IsCasterSettled(kRight, a_pc)) {
            return;  // never deliver into a caster that is still settling
        }

        // Both downs in the SAME frame, left first — the engine then sees one clean simultaneous
        // dual-detection, exactly like the synchronized pair recast.
        const bool okL = EpisodeDeliverHand(kLeft, a_pc);
        const bool okR = EpisodeDeliverHand(kRight, a_pc);
        if (!okL && !okR) {
            EpisodeEnd("gate rejected both hands");
            return;
        }
        spdlog::info("[cc-ep] cast L+R '{}' (L={} R={}, dwell L={:.2f} R={:.2f})",
            g_ep.spell->GetName(), okL ? 1 : 0, okR ? 1 : 0,
            g_hand[kLeft].latchDwell, g_hand[kRight].latchDwell);
    }

    // ---- run gate ---------------------------------
    // Does this hand already have a LIVE engine run? Two ways:
    //   * Latched AND the charge has been OBSERVED to start (PollOnce sets chargeStarted the first
    //     frame the caster leaves kNone) — the engine is actively charging this hand.
    //   * A wind-down still outstanding — the engine is still finishing this hand's previous run.
    // A hand that is Latched but has NOT charged yet is deliberately NOT a live run: that is the
    // engine's dual-decision window, where both casters sit at kNone while it decides whether a
    // dual is forming, and it is exactly when a legitimate pair forms.
    bool HandRunInProgress(std::size_t a_hand)
    {
        return (g_hand[a_hand].phase == Phase::Latched && g_hand[a_hand].chargeStarted) ||
               WindDownPending(a_hand);
    }

    // Run-gate log dedup, same episode model as g_pairVetoLogged.
    bool g_pairRunGateLogged = false;

    // Upgrade to a driving pair ONLY when a real engine dual is observed: GetIsDualCasting()==true
    // on a caster while at least one hand is Latched. Both-latched alone stays two INDEPENDENT
    // latches (handled by per-hand PollOnce). Called BEFORE any per-hand poll so that, once a real
    // dual is active, the partner's expected kNone can never reset it.
    void UpdateDrivePairActivation(RE::PlayerCharacter* a_pc)
    {
        // Every return below that is NOT the veto means the veto does not apply this frame, so it
        // ends the current veto episodeand the next one logs again.
        if (g_drive.active || !a_pc) {
            g_pairVetoLogged = false;
            g_pairRunGateLogged = false;
            return;
        }
        // The DualEvent path coordinates itself — leave those hands to it.
        if (g_hand[kLeft].dualEventHand || g_hand[kRight].dualEventHand) {
            g_pairVetoLogged = false;
            return;
        }

        const bool lLatched = g_hand[kLeft].phase == Phase::Latched;
        const bool rLatched = g_hand[kRight].phase == Phase::Latched;
        if (!lLatched && !rLatched) {
            g_pairVetoLogged = false;
            g_pairRunGateLogged = false;
            return;  // nothing latched -> no pair possible
        }
        // Don't hijack a hand that is mid-Releasing into a fresh pair.
        if (g_hand[kLeft].phase == Phase::Releasing || g_hand[kRight].phase == Phase::Releasing) {
            g_pairVetoLogged = false;
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
            g_pairVetoLogged = false;
            return;
        }

        // RUN GATE. A pair must NEVER be formed across a hand that already
        // has a live engine run. Measured: after a pair break a hand's
        // deferred latch executed a perfectly legitimate SINGLE run (veto -> single latch, charge
        // started, BeginCastRight observed); ~0.3 s later this function formed a fresh two-button
        // pair (btn L=1 R=1) straight across that running cast. The partner was force-latched as
        // driver, but the engine was busy with the running charge, so the driver's charge never
        // started -> 1.0 s grace -> pair break -> the balance release forced the running cast off
        // early. Self-healing but a ~1 s hitch, and it hijacks a live run.
        //
        // Rejecting here changes nothing else: the latched hand simply continues as the normal
        // single latch that PollOnce already drives, and the next clean cycle (both hands back at
        // Idle/settled, i.e. both in the engine's dual-decision window) forms the pair naturally.
        if (HandRunInProgress(kLeft) || HandRunInProgress(kRight)) {
            if (!g_pairRunGateLogged) {
                g_pairRunGateLogged = true;
                spdlog::info("[cc] pair gate (run in progress L={} R={}, wd L={} R={}) -> single latch",
                    (lLatched && g_hand[kLeft].chargeStarted) ? 1 : 0,
                    (rLatched && g_hand[kRight].chargeStarted) ? 1 : 0,
                    WindDownPending(kLeft) ? 1 : 0, WindDownPending(kRight) ? 1 : 0);
            }
            g_pairVetoLogged = false;
            return;
        }
        g_pairRunGateLogged = false;

        // FIX: GetIsDualCasting is NECESSARY but NOT SUFFICIENT. A real engine
        // dual ALWAYS has BOTH physical attack buttons down. A stale kDualCasting flag (left set by
        // an earlier completed dual and not yet cleared by the engine) could otherwise form a FALSE
        // pair on a single click: driver = the OTHER hand, whose caster never charges -> grace
        // timeout -> the click is swallowed with no cast. Require both physical buttons down; if
        // not, veto the pair and let the latched hand continue as a normal single latch (PollOnce).
        // Partner-driver adoption (two-handed scrolls) runs via PollOnce/PartnerActiveSameForm with
        // GetIsDualCasting()==false, so it never reaches here and is unaffected.
        //
        // /: a hand the DUAL-EPISODE CONTROLLER is holding counts as down here —
        // vanilla really is holding that control (the controller fed it a synthetic down). Outside
        // an episode no such hold exists, so the stale-flag protection is bit-for-bit the
        // test — in particular the regression scenario (different spells per hand,
        // single click) is unaffected.
        if (!(HandHeld(kLeft) && HandHeld(kRight))) {
            // Logged ONCE per episode— the check itself is unchanged and still
            // runs every frame, only the line is deduped.
            if (!g_pairVetoLogged) {
                g_pairVetoLogged = true;
                spdlog::info("[cc] pair veto (dual flag L={} R={}, btn L={} R={}, ep L={} R={}) -> single latch",
                    lDual ? 1 : 0, rDual ? 1 : 0, g_phys[kLeft].down ? 1 : 0, g_phys[kRight].down ? 1 : 0,
                    EpisodeHoldsHand(kLeft) ? 1 : 0, EpisodeHoldsHand(kRight) ? 1 : 0);
            }
            return;
        }
        g_pairVetoLogged = false;  // veto passed -> episode over (covers pair formation below)

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
        spdlog::info("[cc] pair L+R {}='{}' (engine dual, driver={}, btn L={} R={}, ep L={} R={}, "
                     "dual L={} R={}, dwell L={:.2f} R={:.2f})",
            KindName(dForm), dForm ? dForm->GetName() : "?", HandName(driver),
            g_phys[kLeft].down ? 1 : 0, g_phys[kRight].down ? 1 : 0,
            EpisodeHoldsHand(kLeft) ? 1 : 0, EpisodeHoldsHand(kRight) ? 1 : 0,
            lDual ? 1 : 0, rDual ? 1 : 0,
            g_hand[kLeft].latchDwell, g_hand[kRight].latchDwell);
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
            if (g_drive.releasingElapsed > kReleasingSafetySecs) {
                EndPair("Releasing timeout (safety cap)");
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
            // One engine run, two releases. Only the DRIVER's per-hand events will arrive, so its
            // SpellFire confirms both fires and its Equipped_Event clears both wind-downs.
            NoteFirePair(drv, other);
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
    // was released during the delay. On expiry the recast additionally waits for the caster to be
    // back at kNoneand only then relatches (re-checks button-held + gate).
    // When BOTH hands are pending it is a dual: they relatch TOGETHER once BOTH casters are at
    // kNone, and both synthetic downs go out in the SAME frame (deterministic order, left first)
    // so the engine gets a clean simultaneous dual-detection instead of a drifted, asynchronous
    // re-latch. A hand that waits past kRecastWaitCapSecs is dropped and the pair degrades.
    // Readiness of one armed pending recast. The pacing delay alone is no
    // longer enough: the hand's caster must also be fully back at kNone before a synthetic down may
    // be delivered.
    enum class RecastGate
    {
        NotDue,   // the 0.25 s pacing delay has not elapsed yet
        Ready,    // delay elapsed AND the caster is at kNone -> relatch now
        Waiting,  // delay elapsed but the engine is still finishing the previous cast
        GiveUp    // waited past kRecastWaitCapSecs -> drop the pending recast, deliver nothing
    };

    RecastGate EvaluateRecastGate(std::size_t a_hand, RE::PlayerCharacter* a_pc, float a_delta)
    {
        auto& PR = g_pending[a_hand];
        if (PR.remaining > 0.0f) {
            return RecastGate::NotDue;
        }

        // Ready needs a SETTLED caster (stable kNone), not a momentary kNone.
        if (IsCasterSettled(a_hand, a_pc)) {
            return RecastGate::Ready;
        }

        auto*      caster = GetCaster(a_pc, a_hand);
        const auto state  = caster ? caster->state.get() : RE::MagicCaster::State::kNone;

        PR.waitElapsed += a_delta;
        if (PR.waitElapsed > kRecastWaitCapSecs) {
            spdlog::info("[cc] recast {} give up (caster never returned to kNone, waited {:.1f}s, state={})",
                HandName(a_hand), PR.waitElapsed, static_cast<std::uint32_t>(state));
            return RecastGate::GiveUp;
        }
        if (!PR.waitLogged) {   // one line per wait episode
            PR.waitLogged = true;
            spdlog::info("[cc] recast wait {} state={}", HandName(a_hand), static_cast<std::uint32_t>(state));
        }
        return RecastGate::Waiting;
    }

    // Drop an armed pending recast without delivering anything.
    void AbandonPendingRecast(std::size_t a_hand)
    {
        auto& PR = g_pending[a_hand];
        PR.active = false;
        PR.waitElapsed = 0.0f;
        PR.waitLogged = false;
    }

    // Poll the deferred latches. Executes as soon as the hand is settled
    // again — same shape as the pending-recast poll. PerformRecast is reused deliberately: it is
    // exactly "start a fresh latch from the tracked physical button and feed vanilla a synthetic
    // down", including the gate re-evaluation.
    void TickDeferredLatches(RE::PlayerCharacter* a_pc)
    {
        for (std::size_t hand = 0; hand < kHandCount; ++hand) {
            if (!g_deferred[hand].active) {
                continue;
            }
            if (!g_phys[hand].down) {           // released before we got the chance
                CancelDeferredLatch(hand, "btn released");
                continue;
            }
            if (g_hand[hand].phase != Phase::Idle) {
                CancelDeferredLatch(hand, "hand no longer Idle");
                continue;
            }
            if (!IsCasterSettled(hand, a_pc)) {
                continue;                       // still winding down / caster not back at kNone
            }

            // Equip must still be what was clicked, otherwise drop instead of casting something else.
            RE::TESForm* current = EvaluateGate(a_pc, hand);
            if (current != g_deferred[hand].spell) {
                spdlog::info("[cc] deferred latch {} dropped (equip changed since click)", HandName(hand));
                ClearDeferredLatch(hand);
                continue;
            }

            const bool ok = PerformRecast(hand, a_pc, "deferred latch");
            spdlog::info("[cc] deferred latch {} {}", HandName(hand), ok ? "executed" : "dropped");
            ClearDeferredLatch(hand);
        }
    }

    void TickPendingRecasts(RE::PlayerCharacter* a_pc, float a_delta)
    {
        // Cancel any hand released during its delay, then decrement the survivors.
        for (std::size_t hand = 0; hand < kHandCount; ++hand) {
            auto& PR = g_pending[hand];
            if (!PR.active) {
                continue;
            }
            // Held = physical button, or a one-click-dual virtual down. The virtual marking is
            // cleared the frame its owning physical hand goes up, so the partner's recast stops
            // in the same cycle as the hand that started it.
            if (!HandHeld(hand)) {  // released during the delay -> cancel
                spdlog::info("[cc] recast {} cancel (btn released)", HandName(hand));
                PR.active = false;
                PR.waitElapsed = 0.0f;
                PR.waitLogged = false;
                continue;
            }
            PR.remaining -= a_delta;
        }

        const bool lPending = g_pending[kLeft].active;
        const bool rPending = g_pending[kRight].active;

        if (lPending && rPending) {
            // Dual pending: both hands relatch TOGETHER. BOTH gates are evaluated before EITHER
            // down goes out, so the engine still sees one clean simultaneous dual — and so the
            // decision can never be taken on a state that the partner's own down has just changed.
            const RecastGate lGate = EvaluateRecastGate(kLeft, a_pc, a_delta);
            const RecastGate rGate = EvaluateRecastGate(kRight, a_pc, a_delta);

            if (lGate == RecastGate::GiveUp || rGate == RecastGate::GiveUp) {
                if (lGate == RecastGate::GiveUp) {
                    AbandonPendingRecast(kLeft);
                }
                if (rGate == RecastGate::GiveUp) {
                    AbandonPendingRecast(kRight);
                }
                return;  // whatever survives is handled by the single-pending path next tick
            }
            if (lGate != RecastGate::Ready || rGate != RecastGate::Ready) {
                return;  // still pacing, or one caster is not back at kNone -> keep both armed
            }

            g_pending[kLeft].active = false;
            g_pending[kRight].active = false;
            PerformRecast(kLeft, a_pc, "hold-to-recast pair (delay elapsed, both kNone)");   // left first
            PerformRecast(kRight, a_pc, "hold-to-recast pair (delay elapsed, both kNone)");
            return;
        }

        // Single pending hand.
        for (const std::size_t hand : { kLeft, kRight }) {
            auto& PR = g_pending[hand];
            if (!PR.active) {
                continue;
            }
            const RecastGate gate = EvaluateRecastGate(hand, a_pc, a_delta);
            if (gate == RecastGate::GiveUp) {
                AbandonPendingRecast(hand);
                continue;
            }
            if (gate != RecastGate::Ready) {
                continue;  // NotDue / Waiting -> stay armed
            }
            PR.active = false;
            PerformRecast(hand, a_pc, "hold-to-recast (delay elapsed, kNone)");
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

        // Idle dwell FIRST, so every gate consulted later in this frame —
        // recast, pair activation, and the input hook until the next frame — reads the same value.
        TickIdleDwell(pc, a_delta);

        TickAnimSinkRegistration(a_delta);          // keeps the event sink armed
        TickWindDownFailsafe(a_delta);              // emergency valve for a missed CastStop
        TickFireConfirm(a_delta);                   // detection only
        TickPhysHandoffGrace(a_delta);              // physical-activity window per hand
        TickDeferredLatches(pc);                    // swallowed clicks waiting for the wind-down

        // the bow machine. Independent of everything below it — it shares no state
        // with the spell paths, so its position in this sequence is not load-bearing.
        TickBowEpisode(pc, a_delta);

        // Hold-to-recast delay: expire pending recasts BEFORE pairing/polling, so any relatch
        // (and its synthetic down) is in place before a driving pair can re-form.
        TickPendingRecasts(pc, a_delta);

        // the dual-episode controller runs immediately before pair activation, so the two
        // synthetic downs it delivers are in place when the pair forms in this same frame.
        TickDualEpisode(pc, a_delta);

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

    // ---- dual-attack control  ------------------------------------------
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
        L.latchDwell = g_idleDwell[a_hand];
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

                // Pass the dual-DOWN to vanilla so it begins charging BOTH casters. Swallowing it
                // entirely would leave vanilla with nothing to charge, exactly as the single-hand
                // latch passes its own down through. Deliberate: mirror single-hand — charge on
                // down, swallow only the dual-UP virtual hold. Payload captured above.
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
                g_swallowNextUp[hand] = false;   // a fresh down starts a new ledger
            } else if (isUp) {
                g_phys[hand].down = false;
                g_physRecentUp[hand] = kPhysHandoffGraceSecs;  // hand-off grace starts now
            }
        }

        // this hand's delivered down was already balanced for us while the button was still
        // held (pair-break balance), so vanilla is holding nothing — swallow the physical up that
        // would otherwise be a second release for one down.
        if (hand != kHandCount && isUp && g_swallowNextUp[hand]) {
            g_swallowNextUp[hand] = false;
            return;
        }

        // During a pending recast the synthetic UP has ALREADY been delivered to vanilla, so the
        // physical up/held for this hand must be swallowed — passing it through would be a second
        // up (double release). A fresh physical DOWN is allowed through: it cleared the pending
        // above and latches normally below.
        if (hand != kHandCount && g_pending[hand].active && !isDown) {
            return;  // normal during a pending recast — the cancel/fire line reports the outcome
        }

        // A deferred latch swallowed this hand's physical DOWN, so vanilla
        // is holding nothing for it — the matching up/held must be swallowed too, and the up ends
        // the wait. Nothing was ever delivered, so nothing is owed either way.
        if (hand != kHandCount && g_deferred[hand].active && !isDown) {
            if (isUp) {
                CancelDeferredLatch(hand, "btn released");
            }
            return;
        }

        // ---- BOW AUTO-SHOT -----------------------------------------------
        // Dispatched BEFORE any spell path so a ranged weapon can never enter a spell latch. The
        // bow machine returns NotBow for everything it does not own (no bow equipped, toggle off,
        // no ammo, hand mid-latch), and the spell path below then runs bit-for-bit unchanged —
        // which for a bow ends in EvaluateGate's bow pass-through.
        if (hand != kHandCount) {
            switch (ProcessBowButton(hand, a_event, a_data, isDown, isUp)) {
            case BowAction::Swallow:
                return;  // do NOT call original — we owe vanilla the matching up
            case BowAction::PassThrough:
                CallOriginal(a_this, a_event, a_data);
                return;
            case BowAction::NotBow:
                break;  // fall through into the unchanged spell machine
            }
        }

        // "Dual Attack" control  has its own handling.
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

                const auto cState = caster ? caster->state.get() : RE::MagicCaster::State::kNone;

                // Wind-down: this hand's PREVIOUS run has not reported its
                // CastStop yet. SWALLOW the click and remember it as a deferred latch — passing it
                // to vanilla makes vanilla start and abort a charge, which keeps the animation
                // graph in its cast state and pushes CastStop even further out (the 23:22 spam
                // loop). Nothing is delivered here, so nothing is owed.
                if (WindDownPending(hand)) {
                    g_busyLogged[hand] = false;  // this episode is handled, not passed through
                    ArmDeferredLatch(hand, spell, "wind-down");
                    return;  // do NOT call original
                }

                // Any OTHER busy reason (caster not back at kNone, no wind-down outstanding) keeps
                // the old behaviour: hand the click to vanilla untouched, exactly as this hand
                // behaved before the mod existed. One line per busy episode; dwell= is measurement.
                if (!IsCasterSettled(hand, pc)) {
                    if (!g_busyLogged[hand]) {
                        g_busyLogged[hand] = true;
                        spdlog::info("[cc] busy {} state={}({}) wd=0 dwell={:.2f} -> pass-through (caster not idle)",
                            HandName(hand), static_cast<std::uint32_t>(cState), StateName(cState),
                            g_idleDwell[hand]);
                    }
                    CallOriginal(a_this, a_event, a_data);
                    return;
                }
                g_busyLogged[hand] = false;
                ClearDeferredLatch(hand);  // a fresh click that latches now supersedes any wait

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
                L.latchDwell = g_idleDwell[hand];

                // The one lifecycle line for a latch — the gate PASS is implicit in it. dwell= is
                // the measured idle dwell that satisfied the settled gate.
                spdlog::info("[cc] latch {} {}='{}' ct={:.2f} dwell={:.2f}",
                    HandName(hand), KindName(spell), spell->GetName(), chargeTime, L.latchDwell);

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

    // ========================================================================
    // INPUT FIREWALL— PlayerControls' BSTEventSink<InputEvent*>::ProcessEvent
    //
    // VTABLE_PlayerControls[0] (RE\Offsets_VTABLE.h:2348) is the BSTEventSink<InputEvent*> base at
    // offset 0x000; ProcessEvent is slot 0x1 of that base (RE\B\BSTEvent.h:146). It receives the
    // head of the frame's whole InputEvent chain BEFORE any PlayerInputHandler runs, so removing an
    // event here means no handler — vanilla's or ours — ever sees it.
    //
    // Only the SAME-SPELL configuration is touched, and within it only mouse/keyboard ButtonEvents
    // on the two attack controls. Everything else passes through bit-identically; the original is
    // ALWAYS called.
    //
    // Chain surgery (verified in the headers): the parameter is InputEvent* const*, so the HEAD
    // POINTER itself cannot be reassigned — but the InputEvent objects are non-const, so
    // InputEvent::next (RE\I\InputEvent.h:43) is writable. A non-head event is therefore unlinked
    // normally. A HEAD event that must be swallowed is instead NEUTRALISED by clearing its
    // userEvent to the empty BSFixedString. That is the gentlest technique available: routing is
    // done purely by userEvent (PlayerInputHandler::CanProcess), so no handler claims it; the event
    // stays structurally intact so the chain walk is unaffected; and it is self-healing —
    // BSInputEventQueue::AddButtonEvent assigns `cachedEvent.userEvent = {}` into the very same
    // fixed array slot every frame before ControlMap maps it again, so an empty userEvent is a
    // state the engine itself writes and can never be left stuck.
    //
    // Events are NEVER injected: the queue owns its objects (fixed arrays in BSInputEventQueue).
    // The controller delivers only through ButtonEvent::Create -> g_originalProcessButton.
    using ProcessInputFn = RE::BSEventNotifyControl(RE::BSTEventSink<RE::InputEvent*>*,
                                                   RE::InputEvent* const*,
                                                   RE::BSTEventSource<RE::InputEvent*>*);
    REL::Relocation<ProcessInputFn> g_originalProcessInput;

    // Classify an event as one of our attack controls; returns kHandCount for everything else.
    std::size_t FirewallHandOf(RE::InputEvent* a_event)
    {
        if (!a_event || a_event->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) {
            return kHandCount;
        }
        if (a_event->GetDevice() == RE::INPUT_DEVICE::kGamepad) {
            return kHandCount;  // gamepad is always untouched (spec)
        }
        auto* userEvents = RE::UserEvents::GetSingleton();
        if (!userEvents) {
            return kHandCount;
        }
        const RE::BSFixedString& ue = a_event->QUserEvent();
        if (ue == userEvents->leftAttack) {
            return kLeft;
        }
        if (ue == userEvents->rightAttack) {
            return kRight;
        }
        return kHandCount;
    }

    // Record what the player wants, and nothing else. g_phys is deliberately NOT touched here:
    // during the configuration the controller is the sole owner of these hands, and it normalises
    // g_phys itself at episode start.
    void FirewallNoteDemand(std::size_t a_hand, RE::ButtonEvent* a_button, RE::PlayerControlsData* a_data)
    {
        g_ep.silence[a_hand] = 0.0f;   // any event at all proves the stream is alive
        g_ep.device = a_button->device.get();
        g_ep.idCode[a_hand] = a_button->GetIDCode();
        if (a_data) {
            g_ep.data = a_data;
        }

        const bool isDown = a_button->value > 0.0f && a_button->heldDownSecs == 0.0f;
        const bool isUp   = a_button->value == 0.0f && a_button->heldDownSecs > 0.0f;
        if (isDown && !g_ep.demand[a_hand]) {
            g_ep.demand[a_hand] = true;
            EpisodeLogDemand(a_hand, true);
        } else if (isUp && g_ep.demand[a_hand]) {
            g_ep.demand[a_hand] = false;
            EpisodeLogDemand(a_hand, false);
        }
        // Held continuations only refresh the silence clock (above).
    }

    RE::BSEventNotifyControl ProcessInput_Hook(RE::BSTEventSink<RE::InputEvent*>*  a_this,
                                              RE::InputEvent* const*              a_event,
                                              RE::BSTEventSource<RE::InputEvent*>* a_source)
    {
        if (!a_event || !*a_event) {
            return g_originalProcessInput(a_this, a_event, a_source);
        }

        // GAMEPLAY GUARD. The firewall may only ever eat events that are
        // gameplay attack input. In menu mode the same controls drive menu navigation, so removing
        // them there would swallow the player's UI input; the firewall is therefore fully inert and
        // the chain passes through bit-identically.
        //
        // Header-verified (vcpkg CommonLibSSE-NG 3.7.0 include tree), RE/U/UI.h:
        //   :79   static UI* GetSingleton();
        //   :83   bool GameIsPaused();        <- numPausesGame != 0  (member :129, imenu flag 0x00001)
        //   :90   bool IsItemMenuOpen() const;<- numItemMenus != 0   (member :130, imenu flag 0x02000)
        //   :92   bool IsModalMenuOpen() const;<- modal              (member :136, imenu flag 0x00010)
        // Those three counters are exactly the "a menu owns the game right now" set: a pausing
        // menu, an item menu (inventory/magic/barter/container), or a modal one. There is no
        // single boolean "menu mode" accessor on UI — the menu-mode counter is only exposed as the
        // BSTEventSource<void*> at offset 0B8 (:58) with no getter — so this trio IS the check.
        auto* ui = RE::UI::GetSingleton();
        if (!ui || ui->GameIsPaused() || ui->IsItemMenuOpen() || ui->IsModalMenuOpen()) {
            return g_originalProcessInput(a_this, a_event, a_source);  // untouched
        }

        // There is no separate dual-cast toggle any more: dual casting is
        // part of Spell Casting, so the only remaining gate is the configuration test below — and that
        // runs through EvaluateGate, which already honours bSpellCasting. With the type disabled,
        // SameSpellBothHands returns nullptr and the firewall is inert exactly as before.
        auto* pc = RE::PlayerCharacter::GetSingleton();
        // Configuration is resolved ONCE per chain (i.e. once per frame), not per event.
        if (!SameSpellBothHands(pc)) {
            return g_originalProcessInput(a_this, a_event, a_source);  // untouched
        }

        auto* controls = RE::PlayerControls::GetSingleton();
        RE::PlayerControlsData* data = controls ? &controls->data : nullptr;

        // Head first: it cannot be unlinked, so it is neutralised in place.
        std::size_t headHand = FirewallHandOf(*a_event);
        if (headHand != kHandCount) {
            auto* button = (*a_event)->AsButtonEvent();
            if (button) {
                FirewallNoteDemand(headHand, button, data);
                button->userEvent = RE::BSFixedString();   // no handler can route it now
            }
        }

        // Then the tail: ordinary unlinking through the writable next pointers.
        RE::InputEvent* prev = *a_event;
        while (prev && prev->next) {
            RE::InputEvent* cur  = prev->next;
            const std::size_t h  = FirewallHandOf(cur);
            if (h == kHandCount) {
                prev = cur;
                continue;
            }
            auto* button = cur->AsButtonEvent();
            if (button) {
                FirewallNoteDemand(h, button, data);
            }
            prev->next = cur->next;   // unlink; the queue still owns the object
        }

        return g_originalProcessInput(a_this, a_event, a_source);
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

        // input firewall. VTABLE_PlayerControls[0] is the BSTEventSink<InputEvent*> base
        // (offset 0x000, RE\Offsets_VTABLE.h:2348); ProcessEvent is slot 0x1 of that base
        // (RE\B\BSTEvent.h:146). PlayerControls does not declare the override in its header, so the
        // slot index comes from the base.
        REL::Relocation<std::uintptr_t> vtblControls{ RE::VTABLE_PlayerControls[0] };
        g_originalProcessInput = vtblControls.write_vfunc(0x1, &ProcessInput_Hook);

        g_installed = true;

        spdlog::info("[cc] install: hooked AttackBlockHandler::ProcessButton (vtbl 4) + "
                     "PlayerCharacter::Update (vtbl 0xAD) + PlayerControls InputEvent sink (vtbl 1)");

        // first attempt at kDataLoaded. The player's animation graph may not
        // exist yet here, which is fine — TickAnimSinkRegistration retries every 5 s from the
        // update hook until it takes, and keeps re-arming after load-game / new-game / race change.
        TickAnimSinkRegistration(kAnimRegisterRetrySecs + 1.0f);
    }
}
