#pragma once

#include "SettingsPreviewPage.g.h"

namespace winrt::FastLook::implementation
{
    struct SettingsPreviewPage : SettingsPreviewPageT<SettingsPreviewPage>
    {
        SettingsPreviewPage();

    private:
        void Build();

        Microsoft::UI::Xaml::Controls::InfoBar m_error{nullptr};
    };
}

namespace winrt::FastLook::factory_implementation
{
    struct SettingsPreviewPage : SettingsPreviewPageT<SettingsPreviewPage, implementation::SettingsPreviewPage>
    {
    };
}
