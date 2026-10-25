#pragma once

#include <functional>
#include <string_view>

namespace fastlook::viewer
{
    // Opens the settings window on `page` (general|preview|about; empty = general). The window is created on demand and
    // destroyed when it is closed; onClosed then runs exactly once. While it is open a second call only re-activates it
    // (its onClosed is dropped).
    void ShowSettingsWindow(std::wstring_view page, std::function<void()> onClosed);
    // Closes the settings window if it is open (the exit sequence); its onClosed runs as for a user close.
    void CloseSettingsWindow();
    // Re-applies the theme setting to the open settings window.
    void RefreshSettingsWindowTheme();
}
