#pragma once

#include <string_view>

#include "Settings/Settings.h"

namespace fastlook::daemon
{
    enum class StrId
    {
        TraySettings,
        TrayPause,
        TrayResume,
        TrayExit,
        Tooltip,
        TooltipPaused,
        PeekTitle,
        PeekText
    };

    // Daemon UI strings (EN / RU). The daemon has no WinRT, so these live in code instead of .resw.
    std::wstring_view S(StrId id);
    // System resolves to Russian when the user's UI language is Russian, English otherwise.
    void SetLanguage(core::LanguagePref pref);
}
