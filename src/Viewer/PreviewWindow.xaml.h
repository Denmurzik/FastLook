#pragma once

#include "PreviewWindow.g.h"

namespace winrt::FastLook::implementation
{
    struct PreviewWindow : PreviewWindowT<PreviewWindow>
    {
        PreviewWindow();

        HWND Hwnd() const noexcept { return m_hwnd; }
        // Rasterization scale of the monitor the window is on (1.0 = 96 dpi).
        double Scale() const;

        // Loading state: the "Loading..." text over an empty content host.
        void ShowLoading();
        // Replaces the hosted view; hides the loading text.
        void SetContent(Microsoft::UI::Xaml::UIElement const& view);
        void ClearContent();
        void SetTitleText(hstring const& text);
        void SetTheme(Microsoft::UI::Xaml::ElementTheme theme);

    private:
        HWND m_hwnd = nullptr;
        Microsoft::UI::Xaml::UIElement m_content{nullptr};
    };
}

namespace winrt::FastLook::factory_implementation
{
    struct PreviewWindow : PreviewWindowT<PreviewWindow, implementation::PreviewWindow>
    {
    };
}
