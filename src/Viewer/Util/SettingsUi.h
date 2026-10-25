#pragma once

#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "Settings/Settings.h"

namespace fastlook::viewer
{
    // Applies `edit` to a copy of the effective settings and saves it at once (every change in the settings window is
    // saved immediately). False when nothing was written.
    bool CommitSettings(std::function<void(core::Settings&)> const& edit);

    // One Windows 11 settings row: title and description on the left, `control` on the right, in a card.
    winrt::Microsoft::UI::Xaml::Controls::Grid MakeSettingsRow(winrt::hstring const& title, winrt::hstring const& description,
                                                               winrt::Microsoft::UI::Xaml::UIElement const& control);

    // A toggle with localized On/Off labels. `apply` runs on a user change and returns false to put the switch back.
    winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch MakeToggle(bool on, std::function<bool(bool)> apply);

    // A combo box over `items`. `apply` runs with the new index on a user change and returns false to roll the selection back.
    winrt::Microsoft::UI::Xaml::Controls::ComboBox MakeCombo(std::initializer_list<winrt::hstring> items, int selected,
                                                             std::function<bool(int)> apply);

    // A closed InfoBar for the page; ShowSaveResult opens it with the "couldn't save" text when `saved` is false.
    winrt::Microsoft::UI::Xaml::Controls::InfoBar MakeErrorBar();
    // Event handlers hold the bar weakly: the bar's parent owns the control that owns the handler.
    using WeakBar = winrt::weak_ref<winrt::Microsoft::UI::Xaml::Controls::InfoBar>;
    void ShowError(WeakBar const& bar, std::wstring_view key);
    void ShowSaveResult(WeakBar const& bar, bool saved);

    // Replaces "{0}" in `pattern`.
    winrt::hstring FormatOne(winrt::hstring const& pattern, std::wstring const& value);
}
