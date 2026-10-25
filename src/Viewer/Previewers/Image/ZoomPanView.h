#pragma once

#include <functional>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

#include "Geometry.h"
#include "Previewers/IPreviewer.h"

namespace fastlook::viewer
{
    // Fit-to-window image with zoom (Ctrl+wheel, pinch, ZoomCommand) and drag panning. Zoom factor 1 is "fit"; images smaller
    // than the viewport are shown at 1:1 pixels and centred. UI thread only.
    class ZoomPanView
    {
    public:
        ZoomPanView();
        ~ZoomPanView();
        ZoomPanView(ZoomPanView const&) = delete;
        ZoomPanView& operator=(ZoomPanView const&) = delete;

        winrt::Microsoft::UI::Xaml::UIElement Root();

        // `pixels` is the full-resolution size of the content (after orientation), `scale` is DIP -> pixel (monitor DPI / 96).
        void SetSource(winrt::Microsoft::UI::Xaml::Media::ImageSource const& source, core::SizeU pixels, double scale);
        // Swaps the bitmap (e.g. a re-decode at the current zoom) without touching zoom, pan or layout.
        void ReplaceSource(winrt::Microsoft::UI::Xaml::Media::ImageSource const& source);
        void Zoom(ZoomCommand command);

        double ZoomFactor() const;
        // Image size at zoom 1, in DIP, and the viewport, in DIP.
        winrt::Windows::Foundation::Size FittedDip() const { return m_fitted; }
        winrt::Windows::Foundation::Size ViewportDip() const { return m_viewport; }

        // Detaches events and drops the source.
        void Reset();

        // Called on the UI thread ~150 ms after the zoom factor stopped changing.
        std::function<void(double zoomFactor)> zoomSettled;

    private:
        void Relayout();
        void ZoomTo(double factor);
        void OnViewChanged(bool intermediate);

        winrt::Microsoft::UI::Xaml::Controls::ScrollViewer m_scroll{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::Grid m_stage{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::Image m_image{nullptr};
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_timer{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::ScrollViewer::ViewChanged_revoker m_viewChanged;
        winrt::Microsoft::UI::Xaml::FrameworkElement::SizeChanged_revoker m_sizeChanged;
        winrt::Microsoft::UI::Xaml::UIElement::PointerPressed_revoker m_pressed;
        winrt::Microsoft::UI::Xaml::UIElement::PointerMoved_revoker m_moved;
        winrt::Microsoft::UI::Xaml::UIElement::PointerReleased_revoker m_released;
        winrt::Microsoft::UI::Xaml::UIElement::PointerCanceled_revoker m_canceled;
        winrt::event_token m_tick{};

        core::SizeU m_pixels{};
        double m_scale = 1.0;
        double m_settledZoom = 1.0;
        double m_targetZoom = 1.0; // where the last ZoomTo is heading: commands issued before the view catches up compound
        winrt::Windows::Foundation::Size m_fitted{};
        winrt::Windows::Foundation::Size m_viewport{};
        bool m_dragging = false;
        winrt::Windows::Foundation::Point m_dragLast{};
    };
}
