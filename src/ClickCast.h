#pragma once

// Click-Casting core: single-hand latch / auto-release, symmetric dual, hold-to-recast.
//
// Install() rewrites AttackBlockHandler::ProcessButton (vtable slot 4) to intercept
// left/right attack input. On a gated click (equipped fire-and-forget spell OR scroll with
// cast time > 0) it latches a "virtual hold": the physical button-up is swallowed and, once
// the hand's magic caster reaches Ready, a synthetic release is delivered so the spell
// fires. Everything else is pass-through. Call once, after the game is fully loaded
// (SKSE kDataLoaded).
//
// Hold-to-recast: the physical button state per hand is tracked separately
// from the latch. When a firing completes (Releasing -> Idle) with the button still held,
// a synthetic UP is delivered to vanilla first — balancing its hold and stopping the engine's
// own auto-recharge (which would otherwise start because the key is still physically down) so
// the mod owns the recast tempo. Then, after a brief 0.25 s pause, a fresh latch is auto-started
// with a synthetic down (a dual fires both hands' downs in the same frame). A latch/pair that
// dies without a delivered release instead balances vanilla with a cleanup release (button up),
// and a wedged driving pair is aborted by a stall guard. Exactly one up per down reaches vanilla
// in every flow. Plain click (press-release) is unchanged — it receives no synthetic up.
//
// Two-handed scrolls: a scroll auto-equipped in both hands runs its cast
// on only ONE caster, so a latch whose own caster stays kNone while the partner (same form) is
// actively casting ADOPTS the partner as its driver — releasing at the partner's kReady — and
// cleanup is suppressed while that partner cast runs, so it is not cancelled.
//
// Logging: one compact "[cc]" line per event. A normal click is two lines
// (latch, fire); a held recast cycle adds two (recast armed, fire). Normal caster-state
// progression, swallowed events and synthetic up/down deliveries are implicit and not logged.
// Deviations (interrupt, grace timeout, stall, cleanup, adoption, pair break) each keep a full
// line with their reason and state, gate rejections log once per equipped form per hand, and
// error paths (null handler/event, hook failure) stay fully verbose at error level.
//
// Staves: fire-and-forget staves are click-cast too. In-game testing on a heavily modded setup
// showed a FaF staff drives the hand caster through the same kUnk->kReady sequence as a spell,
// so the gate accepts a Weapon that IsStaff() with a FaF enchantment (formEnchanting) and
// chargeTime > 0, and the whole machine (latch / poll / release / hold-to-recast / cleanup) is
// reused 1:1 — the staff weapon is tracked as the equipped object. Concentration staves and
// un-enchanted / other weapons are rejected by the gate and pass through.
namespace ClickCast
{
    void Install();
}
