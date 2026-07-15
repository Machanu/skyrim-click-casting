#include <SKSE/SKSE.h>
#include <spdlog/sinks/basic_file_sink.h>

#include "ClickCast.h"

// Resolve the SKSE log directory ourselves. CommonLibSSE-NG's SKSE::log::log_directory()
// reads the "My Games" subfolder name from the runtime via RELOCATION_ID(508778, 380738),
// which on AE 1.6.1170 returns "Skyrim.INI" instead of "Skyrim Special Edition" — sending
// the log to My Games\Skyrim.INI\SKSE\. We build the canonical path from the Documents
// known folder (which honours OneDrive redirection) plus literal folder names instead.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <ShlObj.h>

namespace {
    void SetupLog() {
        wchar_t*   buffer{ nullptr };
        const auto result =
            ::SHGetKnownFolderPath(::FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, std::addressof(buffer));
        std::unique_ptr<wchar_t[], decltype(&::CoTaskMemFree)> knownPath(buffer, ::CoTaskMemFree);
        if (!knownPath || result != S_OK) {
            SKSE::stl::report_and_fail("Failed to get Documents known folder path.");
        }

        std::filesystem::path logsFolder = knownPath.get();
        logsFolder /= "My Games";
        logsFolder /= "Skyrim Special Edition";
        logsFolder /= "SKSE";

        std::error_code ec;
        std::filesystem::create_directories(logsFolder, ec);

        auto pluginName = SKSE::PluginDeclaration::GetSingleton()->GetName();
        auto logFilePath = logsFolder / std::format("{}.log", pluginName);
        auto fileLoggerPtr = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true);
        auto loggerPtr = std::make_shared<spdlog::logger>("global", std::move(fileLoggerPtr));
        spdlog::set_default_logger(std::move(loggerPtr));
        spdlog::set_level(spdlog::level::info);
        spdlog::flush_on(spdlog::level::info);
    }
}

namespace {
    // Install the click-cast hook once the game is fully loaded. kDataLoaded fires after
    // all form data is loaded and the player-control handlers exist, so PlayerControls and
    // the AttackBlockHandler vtable are valid. (kInputLoaded would also work; kDataLoaded is
    // chosen as the safest "everything is initialised" point.)
    void MessageListener(SKSE::MessagingInterface::Message* a_msg) {
        if (a_msg && a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
            ClickCast::Install();
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse);
    SetupLog();

    spdlog::info("Skyrim Click-Casting loaded");

    const auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(MessageListener)) {
        spdlog::error("Failed to register SKSE messaging listener; click-cast hook not installed");
        return false;
    }

    return true;
}
