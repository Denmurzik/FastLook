#pragma once

#include "SettingsAboutPage.g.h"

namespace winrt::FastLook::implementation
{
    struct SettingsAboutPage : SettingsAboutPageT<SettingsAboutPage>
    {
        SettingsAboutPage();

    private:
        void Build();

        Microsoft::UI::Xaml::Controls::InfoBar m_error{nullptr};
    };
}

namespace winrt::FastLook::factory_implementation
{
    struct SettingsAboutPage : SettingsAboutPageT<SettingsAboutPage, implementation::SettingsAboutPage>
    {
    };
}
