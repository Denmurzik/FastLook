#pragma once

#include <functional>
#include <string_view>

namespace fastlook::viewer
{
    // Opens the settings window on `page` (general|preview|about; empty = general); onClosed runs when it is closed.
    // Stub until the settings window task: logs and reports it closed at once.
    void ShowSettingsWindow(std::wstring_view page, std::function<void()> onClosed);
}
