#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Geometry.h"
#include "Settings/Settings.h"

namespace fastlook::core
{
    enum class ViewerMode
    {
        Settings,
        Preview,
        Server,
        Bench
    };

    struct ViewerArgs
    {
        ViewerMode mode = ViewerMode::Settings;
        std::vector<std::wstring> files;
        std::optional<RectI> iconRect;
        std::wstring pipeName;
        bool background = false;
        std::wstring settingsPage;
        std::wstring benchDir;
        uint32_t iterations = 1000;
        std::wstring benchOut;
        std::wstring perfLog;
        std::wstring script;
        std::optional<ThemePref> theme;
        std::optional<LanguagePref> lang;
        std::vector<std::wstring> unknown;
    };

    // argv without argv[0]. Mode precedence: --bench > --preview > --pipe > Settings.
    ViewerArgs ParseViewerArgs(std::span<const std::wstring_view> args);
}
