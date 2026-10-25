#pragma once

#include <atomic>
#include <memory>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "Geometry.h"
#include "Previewers/IPreviewer.h"
#include "Previewers/Shell/HostProcess.h"

namespace fastlook::viewer
{
    // A shell preview handler (IPreviewHandler) hosted by FastLook.PreviewHost.exe. The handler draws into a native child window
    // ("site") of the Viewer window, positioned over a transparent XAML placeholder that follows the layout.
    //
    // A handler window parented under the site shares the Viewer's input queue, so the UI thread never sends to it
    // synchronously while it may hang: the site is moved with SWP_ASYNCWINDOWPOS, the host is asked to quit and then
    // terminated (our own PreviewHost only, never a foreign LocalServer32 process) before the site is destroyed, and a site
    // that still has a foreign child is hidden and parked until that child is gone or responsive.
    class ShellPreviewer final : public IPreviewer
    {
    public:
        winrt::Windows::Foundation::IAsyncAction PrepareAsync(PreviewItem item, std::shared_ptr<PreviewContext const> ctx) override;
        winrt::Microsoft::UI::Xaml::UIElement View() override;
        winrt::Windows::Foundation::Size PreferredSize() const override { return {720, 540}; }
        void Unload() override;

    private:
        void CreateSite();
        void UpdateBounds();
        void FocusRoot();

        std::shared_ptr<PreviewContext const> m_ctx;
        std::shared_ptr<HostProcess> m_host;
        std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
        HWND m_site = nullptr;
        bool m_ready = false;
        bool m_shown = false;
        bool m_unloaded = false;
        core::RectI m_lastRect;
        winrt::Microsoft::UI::Xaml::Controls::Grid m_placeholder{nullptr};
        winrt::Microsoft::UI::Xaml::FrameworkElement::SizeChanged_revoker m_sizeRevoker;
        winrt::Microsoft::UI::Xaml::FrameworkElement::LayoutUpdated_revoker m_layoutRevoker;
        winrt::Microsoft::UI::Xaml::FrameworkElement::Loaded_revoker m_loadedRevoker;
    };
}
