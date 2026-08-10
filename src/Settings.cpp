#include "Settings.h"

#include <RE/Skyrim.h>
#include <SimpleIni.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdlib>

namespace
{
    // Paths are relative to the game's working directory (the Skyrim root), the same
    // convention every SKSE plugin uses for "Data/SKSE/Plugins/...".
    constexpr const char* kDefaultsPath = "Data/MCM/Config/AutoShotClickCasting/settings.ini";
    constexpr const char* kUserPath     = "Data/MCM/Settings/AutoShotClickCasting.ini";

    constexpr const char* kSecFeatures = "Features";
    constexpr const char* kSecTempo    = "Tempo";

    // ---- code defaults = the v1 constants -----------------------------------
    // Every feature on, and the hold-to-recast pacing that used to be the hard-coded
    // kRecastDelaySecs in ClickCast.cpp. With no ini files present these are what runs, so
    // "no MCM installed" is byte-for-byte the old behaviour.
    constexpr bool  kDefFeature     = true;
    // Bow Charge Plus delay is OPT-IN: it deliberately slows the auto-shot down, so it must never
    // turn itself on for someone who does not run that mod.
    constexpr bool  kDefBcpDelay    = false;
    constexpr float kDefRecastDelay = 0.25f;

    constexpr float kRecastDelayMin = 0.10f;
    constexpr float kRecastDelayMax = 1.00f;

    // Threading: Load() runs from the MenuOpenCloseEvent sink, and BSTEventSource guards its
    // dispatch with a BSSpinLock without any header promising WHICH thread delivers (the same
    // situation as the animation-graph sink in ClickCast.cpp). The readers, by contrast, sit in
    // the main-thread hooks. Rather than assume a serialisation nothing proves, every value is
    // an atomic: lock-free scalars, relaxed ordering, so a hot-path read costs a plain load.
    // Settings are independent knobs, so no cross-field consistency is needed and no fence is
    // either.
    std::atomic<bool>  g_spellCasting{ kDefFeature };
    std::atomic<bool>  g_scrollCasting{ kDefFeature };
    std::atomic<bool>  g_staffCasting{ kDefFeature };
    std::atomic<bool>  g_bowAutoShot{ kDefFeature };
    std::atomic<bool>  g_bowChargePlusDelay{ kDefBcpDelay };
    std::atomic<float> g_recastDelay{ kDefRecastDelay };

    // ---- numeric parsing ----------------------------------------------------
    // MCM Helper writes bools as 1/0 (SettingStore.cpp extracts a std::uint32_t), so anything
    // that is not a parseable number is treated as "key not present" and leaves the running
    // value alone rather than silently turning a feature off.
    bool ParseLong(const char* a_raw, long& a_out)
    {
        if (!a_raw || !*a_raw) {
            return false;
        }
        char*      end = nullptr;
        const long v   = std::strtol(a_raw, &end, 10);
        if (end == a_raw) {
            return false;
        }
        a_out = v;
        return true;
    }

    bool ParseFloat(const char* a_raw, float& a_out)
    {
        if (!a_raw || !*a_raw) {
            return false;
        }
        char*      end = nullptr;
        const auto v   = std::strtof(a_raw, &end);
        if (end == a_raw) {
            return false;
        }
        a_out = v;
        return true;
    }

    // Scratch values, filled by the defaults file and then overwritten per key by the user
    // file. Published to the atomics in one go at the end of Load().
    struct Scratch
    {
        bool  spellCasting     = kDefFeature;
        bool  scrollCasting    = kDefFeature;
        bool  staffCasting     = kDefFeature;
        bool  bowAutoShot      = kDefFeature;
        bool  bowChargePlusDelay = kDefBcpDelay;
        float recastDelay      = kDefRecastDelay;
    };

    void ApplyBool(const CSimpleIniA& a_ini, const char* a_section, const char* a_key, bool& a_out)
    {
        long v = 0;
        if (ParseLong(a_ini.GetValue(a_section, a_key, nullptr), v)) {
            a_out = (v != 0);
        }
    }

    // Read one file over the scratch values. A file that does not exist (or cannot be parsed)
    // changes nothing — the caller's current values stand. Returns whether it was read.
    bool ApplyFile(const char* a_path, Scratch& a_out)
    {
        CSimpleIniA ini;
        ini.SetUnicode();
        ini.SetMultiKey(false);
        if (ini.LoadFile(a_path) != SI_OK) {
            return false;
        }

        ApplyBool(ini, kSecFeatures, "bSpellCasting", a_out.spellCasting);
        ApplyBool(ini, kSecFeatures, "bScrollCasting", a_out.scrollCasting);
        ApplyBool(ini, kSecFeatures, "bStaffCasting", a_out.staffCasting);
        ApplyBool(ini, kSecFeatures, "bBowAutoShot", a_out.bowAutoShot);
        ApplyBool(ini, kSecFeatures, "bBowChargePlusDelay", a_out.bowChargePlusDelay);

        float f = 0.0f;
        if (ParseFloat(ini.GetValue(kSecTempo, "fRecastDelay", nullptr), f)) {
            a_out.recastDelay = f;
        }
        return true;
    }

    // ---- reload trigger (b): Journal Menu close ------------------------------
    // Header-verified (vcpkg CommonLibSSE-NG 3.7.0 include tree):
    //   RE/U/UI.h:54-58            class UI : ... public BSTEventSource<MenuOpenCloseEvent>  // 008
    //   RE/U/UI.h:81-82, :147-151  template<class T> void UI::AddEventSink(BSTEventSink<T>*)
    //   RE/U/UI.h:153-157          UI::GetEventSource<T>() == static_cast<BSTEventSource<T>*>(this)
    //   RE/M/MenuOpenCloseEvent.h:7-16  { BSFixedString menuName; bool opening; }
    //   RE/J/JournalMenu.h:30      constexpr static std::string_view MENU_NAME = "Journal Menu";
    //   RE/B/BSFixedString.h:144   operator==(const BSFixedString&, std::basic_string_view<char>)
    //   RE/B/BSTEvent.h:34-56      AddEventSink() de-duplicates (std::find before push_back),
    //                              so re-registering is idempotent and safe.
    //
    // The sink does exactly one thing: re-read the ini files. It touches no latch, pair,
    // episode or timer state — a reload never resets anything that is in flight.
    struct MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent*                  a_event,
                                              RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (a_event && !a_event->opening && a_event->menuName == RE::JournalMenu::MENU_NAME) {
                Settings::Load();
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    MenuSink g_menuSink;
}

namespace Settings
{
    void Load()
    {
        Scratch s{};  // code defaults first, so a removed key really does revert to v1

        const bool haveDefaults = ApplyFile(kDefaultsPath, s);
        const bool haveUser     = ApplyFile(kUserPath, s);

        // Clamp to the ranges the MCM offers, so a hand-edited ini can never push the mod
        // outside the tested envelope.
        if (s.recastDelay < kRecastDelayMin) {
            s.recastDelay = kRecastDelayMin;
        } else if (s.recastDelay > kRecastDelayMax) {
            s.recastDelay = kRecastDelayMax;
        }

        g_spellCasting.store(s.spellCasting, std::memory_order_relaxed);
        g_scrollCasting.store(s.scrollCasting, std::memory_order_relaxed);
        g_staffCasting.store(s.staffCasting, std::memory_order_relaxed);
        g_bowAutoShot.store(s.bowAutoShot, std::memory_order_relaxed);
        g_bowChargePlusDelay.store(s.bowChargePlusDelay, std::memory_order_relaxed);
        g_recastDelay.store(s.recastDelay, std::memory_order_relaxed);

        spdlog::info("[cc] settings (def={} user={}) spell={} scroll={} staff={} "
                     "bow={} bcpDelay={} delay={:.2f}",
            haveDefaults ? 1 : 0, haveUser ? 1 : 0,
            s.spellCasting ? 1 : 0, s.scrollCasting ? 1 : 0, s.staffCasting ? 1 : 0,
            s.bowAutoShot ? 1 : 0, s.bowChargePlusDelay ? 1 : 0,
            s.recastDelay);
    }

    void RegisterMenuSink()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            spdlog::error("[cc] settings: no UI singleton; MCM reload on journal close disabled");
            return;
        }
        ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuSink);
        spdlog::info("[cc] settings: reload sink armed (Journal Menu close)");
    }

    bool  SpellCasting() noexcept { return g_spellCasting.load(std::memory_order_relaxed); }
    bool  ScrollCasting() noexcept { return g_scrollCasting.load(std::memory_order_relaxed); }
    bool  StaffCasting() noexcept { return g_staffCasting.load(std::memory_order_relaxed); }
    bool  BowAutoShot() noexcept { return g_bowAutoShot.load(std::memory_order_relaxed); }
    bool  BowChargePlusDelay() noexcept { return g_bowChargePlusDelay.load(std::memory_order_relaxed); }
    float RecastDelay() noexcept { return g_recastDelay.load(std::memory_order_relaxed); }
}
