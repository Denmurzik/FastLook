#pragma once

#include <string_view>

#include "SettingsWindow.g.h"

namespace winrt::FastLook::implementation
{
    struct SettingsWindow : SettingsWindowT<SettingsWindow>
    {
        SettingsWindow();

        HWND Hwnd() const noexcept { return m_hwnd; }
        // `page` is general|preview|about; anything else selects General.
        void SelectPage(std::wstring_view page);
        // Follows the theme setting (the title bar included).
        void ApplyTheme();

    private:
        void Navigate(hstring const& tag);

        HWND m_hwnd = nullptr;
    };
}

namespace winrt::FastLook::factory_implementation
{
    struct SettingsWindow : SettingsWindowT<SettingsWindow, implementation::SettingsWindow>
    {
    };
}
