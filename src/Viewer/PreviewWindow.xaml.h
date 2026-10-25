#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "PreviewWindow.g.h"
#include "Ui/Commands.h"

namespace fastlook::viewer
{
    class TitleBar;
}

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
        // Window title, title-bar name and counter ("i / n", hidden for one item) and the shell icon of `path`.
        void SetItem(std::wstring const& path, hstring const& name, size_t index, size_t count);
        void SetCounter(size_t index, size_t count);
        void SetToolbar(std::vector<Microsoft::UI::Xaml::UIElement> const& items);
        void SetTheme(Microsoft::UI::Xaml::ElementTheme theme);
        // Caption buttons ignore RequestedTheme: their colours follow the resolved theme.
        void ApplyCaptionColors(bool dark);
        // A pinned window gets minimize/maximize, appears in the switchers and loses its Pin button.
        void SetPinned(bool pinned);
        bool Pinned() const noexcept { return m_pinned; }
        // Full-screen presenter without the title bar; leaving restores the previous bounds.
        bool IsFullscreen() const noexcept { return m_fullscreen; }
        void ToggleFullscreen();

        // Title-bar buttons.
        void SetCommandHandler(std::function<void(fastlook::core::Command)> handler);
        // Window-wide PreviewKeyDown (tunnelling); return true to mark the key handled.
        void SetKeyHandler(std::function<bool(uint32_t vk, bool ctrl, bool shift, bool alt)> handler);

    private:
        void UpdateCaptionInset();

        HWND m_hwnd = nullptr;
        UINT m_insetDpi = 0;
        bool m_pinned = false;
        bool m_fullscreen = false;
        Windows::Graphics::RectInt32 m_windowedBounds{};
        std::optional<bool> m_captionDark;
        std::shared_ptr<fastlook::viewer::TitleBar> m_titleBar;
        std::function<bool(uint32_t, bool, bool, bool)> m_keyHandler;
        Microsoft::UI::Xaml::UIElement m_content{nullptr};
    };
}

namespace winrt::FastLook::factory_implementation
{
    struct PreviewWindow : PreviewWindowT<PreviewWindow, implementation::PreviewWindow>
    {
    };
}
