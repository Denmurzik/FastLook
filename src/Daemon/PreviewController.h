#pragma once

#include <windows.h>

#include <string>
#include <string_view>

#include "Settings/Settings.h"

namespace fastlook::daemon
{
    // Stub until the preview controller lands (Task 29): logs only.
    class PreviewController
    {
    public:
        void Start(HWND mainWnd, core::Settings const& settings, bool warm);
        void Stop();
        void OnTrigger(HWND foreground);
        void ShowPath(std::wstring const& path);
        void TogglePath(std::wstring const& path);
        void OpenSettings(std::wstring_view page = L"general");
        void OnSettingsChanged(core::Settings const& settings);
        void Shutdown();
    };
}
