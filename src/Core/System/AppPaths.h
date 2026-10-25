#pragma once

#include <filesystem>
#include <optional>

namespace fastlook::core
{
    struct AppPaths
    {
        std::filesystem::path exeDir;
        std::filesystem::path dataDir;
        std::filesystem::path settingsFile;
        std::filesystem::path logsDir;
        bool portable = false;
    };

    // Priority: dataDirOverride, then portable (exeDir/portable.txt exists: data lives in exeDir),
    // then localAppData/FastLook.
    AppPaths ResolveAppPaths(std::filesystem::path const& exeDir, std::filesystem::path const& localAppData,
                             std::optional<std::filesystem::path> dataDirOverride);
    // Uses this module's directory, %LOCALAPPDATA% and the FASTLOOK_DATA_DIR environment variable.
    AppPaths CurrentAppPaths();
}
