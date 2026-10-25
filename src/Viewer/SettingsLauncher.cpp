#include "pch.h"
#include "SettingsLauncher.h"

#include "Diagnostics/Log.h"

namespace fastlook::viewer
{
    void ShowSettingsWindow(std::wstring_view, std::function<void()> onClosed)
    {
        FL_LOG_INFO(L"ShowSettingsWindow: not implemented");
        if (onClosed)
        {
            onClosed();
        }
    }
}
