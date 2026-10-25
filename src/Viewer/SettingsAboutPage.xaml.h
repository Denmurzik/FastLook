#pragma once

#include <string>

#include "SettingsAboutPage.g.h"

namespace winrt::FastLook::implementation
{
    struct SettingsAboutPage : SettingsAboutPageT<SettingsAboutPage>
    {
        SettingsAboutPage();

    private:
        void Build();
        fire_and_forget OnCheckUpdates(Microsoft::UI::Xaml::Controls::Button const& button);
        void Open(std::wstring const& path);

        Microsoft::UI::Xaml::Controls::InfoBar m_error{nullptr};
    };
}

namespace winrt::FastLook::factory_implementation
{
    struct SettingsAboutPage : SettingsAboutPageT<SettingsAboutPage, implementation::SettingsAboutPage>
    {
    };
}
