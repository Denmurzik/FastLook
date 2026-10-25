#pragma once

#include "SettingsGeneralPage.g.h"

namespace winrt::FastLook::implementation
{
    struct SettingsGeneralPage : SettingsGeneralPageT<SettingsGeneralPage>
    {
        SettingsGeneralPage();

    private:
        void Build();

        Microsoft::UI::Xaml::Controls::InfoBar m_error{nullptr};
    };
}

namespace winrt::FastLook::factory_implementation
{
    struct SettingsGeneralPage : SettingsGeneralPageT<SettingsGeneralPage, implementation::SettingsGeneralPage>
    {
    };
}
