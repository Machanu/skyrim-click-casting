#pragma once

// MCM-backed runtime settings.
//
// The values are read from the two files MCM Helper itself uses:
//   Data\MCM\Config\AutoShotClickCasting\settings.ini   <- shipped defaults
//   Data\MCM\Settings\AutoShotClickCasting.ini          <- user overrides, rewritten by
//                                                          MCM Helper on every change
// The second file wins per key. A missing file, a missing section or a missing key falls
// back to the previous step, and ultimately to the CODE defaults below — which are exactly
// the constants the mod shipped with before the MCM existed, so an installation without any
// ini files behaves bit-for-bit like v1.
//
// Bools are NUMERIC (1/0), never "true"/"false": MCM Helper's own SettingStore.cpp parses a
// bool by extracting a std::uint32_t from the value, so the string "true" would read as 0 and
// silently disable the feature.
//
// Reload model: no Papyrus binding at all. Load() runs at SKSE
// kDataLoaded and again whenever the Journal Menu closes — the MCM lives inside the journal,
// so closing it is exactly the moment MCM Helper has finished writing the user file.
//
// Threading: the accessors are plain atomic loads, so they are safe from the menu sink as well
// as from the main-thread hooks. See Settings.cpp for why the values have to be atomic at all.
namespace Settings
{
    // Re-read both ini files and publish the result. Idempotent; safe to call repeatedly.
    void Load();

    // Register the MenuOpenCloseEvent sink that re-runs Load() on Journal Menu close.
    // Call at kDataLoaded, when RE::UI's singleton exists. Registration is idempotent.
    void RegisterMenuSink();

    // ---- [Features] --------------------------------------------------------
    bool SpellCasting() noexcept;
    bool ScrollCasting() noexcept;
    bool StaffCasting() noexcept;
    // Dual casting has no toggle of its own — it is part of SpellCasting.
    // Bow auto-shot. Keys that no longer exist here are never queried, so an ini
    // carrying an obsolete key from an earlier version is read without complaint.
    bool BowAutoShot() noexcept;
    // Opt-in, DEFAULT OFF: hold a fully drawn bow a fixed extra moment before the auto-release, so
    // Bow Charge Plus has time to register its first charge level. See kBcpDelaySecs in
    // ClickCast.cpp for the value and the measurement behind it.
    bool BowChargePlusDelay() noexcept;

    // ---- [Tempo] -----------------------------------------------------------
    // Seconds between repeat casts while the attack button is held. Clamped to [0.10, 1.00].
    float RecastDelay() noexcept;
}
