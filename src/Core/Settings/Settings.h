#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "Diagnostics/Log.h"

namespace fastlook::core
{
    enum class ActivationKey
    {
        Space,
        CtrlSpace,
        AltSpace,
        ShiftSpace
    };
    enum class ThemePref
    {
        System,
        Light,
        Dark
    };
    enum class LanguagePref
    {
        System,
        English,
        Russian
    };
    enum class MarkdownView
    {
        Rendered,
        Source
    };

    struct GeneralSettings
    {
        bool desktopEnabled = false;
        ActivationKey activationKey = ActivationKey::Space;
        bool prewarm = true;
        uint32_t idleUnloadMinutes = 30; // 0 = never
        ThemePref theme = ThemePref::System;
        LanguagePref language = LanguagePref::System;
        bool peekWarning = true;
        bool operator==(GeneralSettings const&) const = default;
    };

    struct PreviewSettings
    {
        bool wordWrap = false;
        uint32_t codeFontSize = 13;
        bool autoplayMedia = true;
        bool useShellHandlers = true;
        MarkdownView markdownView = MarkdownView::Rendered;
        double mediaVolume = 0.8;
        bool operator==(PreviewSettings const&) const = default;
    };

    struct Settings
    {
        uint32_t version = 1;
        GeneralSettings general;
        PreviewSettings preview;
        LogLevel logLevel = LogLevel::Info;
        bool operator==(Settings const&) const = default;
    };

    struct SettingsDocument
    {
        Settings values;
        std::string preservedJson = "{}"; // the whole parsed document, so unknown fields survive a save
        bool recoveredFromCorrupt = false;
    };

    inline constexpr uint32_t kSettingsSchemaVersion = 1;

    // Never fails: invalid input yields defaults with recoveredFromCorrupt = true.
    SettingsDocument ParseSettings(std::string_view json);
    std::string SerializeSettings(SettingsDocument const& doc);
    // A missing file yields defaults; a corrupt file is copied to <file>.bak and defaults are used.
    SettingsDocument LoadSettings(std::filesystem::path const& file);
    // Writes <file>.tmp, then replaces <file> atomically.
    bool SaveSettings(std::filesystem::path const& file, SettingsDocument const& doc);
}
