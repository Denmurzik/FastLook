#pragma once

#include "SettingsGeneralPage.g.h"
#include "Settings/Settings.h"

namespace winrt::FastLook::implementation
{
    struct SettingsGeneralPage : SettingsGeneralPageT<SettingsGeneralPage>
    {
        SettingsGeneralPage();

    private:
        void Build();

        void RefreshPeekWarning();

        Microsoft::UI::Xaml::Controls::InfoBar m_error{nullptr};
        Microsoft::UI::Xaml::Controls::InfoBar m_peek{nullptr};
        hstring m_peekPattern;
        fastlook::core::ActivationKey m_key{};
    };
}

namespace winrt::FastLook::factory_implementation
{
    struct SettingsGeneralPage : SettingsGeneralPageT<SettingsGeneralPage, implementation::SettingsGeneralPage>
    {
    };
}
