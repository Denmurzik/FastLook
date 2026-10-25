#include "pch.h"
#include "Previewers/Image/ZoomPanView.h"

#include <algorithm>
#include <chrono>

namespace fastlook::viewer
{
    namespace
    {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        namespace Wf = winrt::Windows::Foundation;

        constexpr double kZoomStep = 1.25;
        constexpr double kMaxZoomOverFit = 8.0;
        constexpr auto kSettleDelay = std::chrono::milliseconds(150);

        Wf::IReference<double> Box(double value)
        {
            return winrt::box_value(value).as<Wf::IReference<double>>();
        }
    }

    ZoomPanView::ZoomPanView()
    {
        m_image = Image{};
        m_image.Stretch(winrt::Microsoft::UI::Xaml::Media::Stretch::Fill);
        m_image.HorizontalAlignment(HorizontalAlignment::Center);
        m_image.VerticalAlignment(VerticalAlignment::Center);

        m_stage = Grid{};
        m_stage.Children().Append(m_image);

        m_scroll = ScrollViewer{};
        m_scroll.ZoomMode(ZoomMode::Enabled);
        m_scroll.HorizontalScrollMode(ScrollMode::Enabled);
        m_scroll.VerticalScrollMode(ScrollMode::Enabled);
        m_scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Hidden);
        m_scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Hidden);
        m_scroll.IsTabStop(false);
        m_scroll.MinZoomFactor(1.0f);
        m_scroll.MaxZoomFactor(static_cast<float>(kMaxZoomOverFit));
        m_scroll.Content(m_stage);

        m_timer = m_scroll.DispatcherQueue().CreateTimer();
        m_timer.IsRepeating(false);
        m_timer.Interval(kSettleDelay);
        m_tick = m_timer.Tick([this](auto const&, auto const&) {
            double const zoom = ZoomFactor();
            if (std::abs(zoom - m_settledZoom) > 1e-3)
            {
                m_settledZoom = zoom;
                if (zoomSettled)
                {
                    zoomSettled(zoom);
                }
            }
        });

        m_viewChanged = m_scroll.ViewChanged(winrt::auto_revoke, [this](auto const&, ScrollViewerViewChangedEventArgs const& e) {
            OnViewChanged(e.IsIntermediate());
        });
        m_sizeChanged = m_scroll.SizeChanged(winrt::auto_revoke, [this](auto const&, auto const&) { Relayout(); });

        m_pressed = m_scroll.PointerPressed(winrt::auto_revoke, [this](auto const&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e) {
            auto const point = e.GetCurrentPoint(m_scroll);
            if (e.Pointer().PointerDeviceType() == winrt::Microsoft::UI::Input::PointerDeviceType::Mouse && point.Properties().IsLeftButtonPressed() &&
                ZoomFactor() > 1.001 && m_scroll.CapturePointer(e.Pointer()))
            {
                m_dragging = true;
                m_dragLast = point.Position();
            }
        });
        m_moved = m_scroll.PointerMoved(winrt::auto_revoke, [this](auto const&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e) {
            if (!m_dragging)
            {
                return;
            }
            auto const position = e.GetCurrentPoint(m_scroll).Position();
            m_scroll.ChangeView(Box(m_scroll.HorizontalOffset() - (position.X - m_dragLast.X)), Box(m_scroll.VerticalOffset() - (position.Y - m_dragLast.Y)),
                                nullptr, true);
            m_dragLast = position;
        });
        auto endDrag = [this](auto const&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e) {
            if (m_dragging)
            {
                m_dragging = false;
                m_scroll.ReleasePointerCapture(e.Pointer());
            }
        };
        m_released = m_scroll.PointerReleased(winrt::auto_revoke, endDrag);
        m_canceled = m_scroll.PointerCanceled(winrt::auto_revoke, endDrag);
    }

    ZoomPanView::~ZoomPanView()
    {
        Reset();
    }

    UIElement ZoomPanView::Root()
    {
        return m_scroll;
    }

    void ZoomPanView::SetSource(winrt::Microsoft::UI::Xaml::Media::ImageSource const& source, core::SizeU pixels, double scale)
    {
        m_pixels = pixels;
        m_scale = scale > 0 ? scale : 1.0;
        m_image.Source(source);
        Relayout();
    }

    void ZoomPanView::ReplaceSource(winrt::Microsoft::UI::Xaml::Media::ImageSource const& source)
    {
        m_image.Source(source);
    }

    void ZoomPanView::Zoom(ZoomCommand command)
    {
        double const current = m_targetZoom;
        switch (command)
        {
        case ZoomCommand::In:
            ZoomTo(current * kZoomStep);
            break;
        case ZoomCommand::Out:
            ZoomTo(current / kZoomStep);
            break;
        case ZoomCommand::Reset:
            ZoomTo(1.0);
            break;
        }
    }

    double ZoomPanView::ZoomFactor() const
    {
        return m_scroll ? m_scroll.ZoomFactor() : 1.0;
    }

    void ZoomPanView::Reset()
    {
        if (m_timer)
        {
            m_timer.Stop();
            m_timer.Tick(m_tick);
            m_timer = nullptr;
        }
        m_viewChanged.revoke();
        m_sizeChanged.revoke();
        m_pressed.revoke();
        m_moved.revoke();
        m_released.revoke();
        m_canceled.revoke();
        zoomSettled = nullptr;
        if (m_image)
        {
            m_image.Source(nullptr);
        }
    }

    void ZoomPanView::Relayout()
    {
        double const vw = m_scroll.ActualWidth();
        double const vh = m_scroll.ActualHeight();
        if (vw <= 0 || vh <= 0 || m_pixels.w == 0 || m_pixels.h == 0)
        {
            return;
        }
        m_viewport = {static_cast<float>(vw), static_cast<float>(vh)};
        m_stage.Width(vw);
        m_stage.Height(vh);

        // Never upscale: an image smaller than the viewport stays at 1:1 pixels.
        double const naturalW = m_pixels.w / m_scale;
        double const naturalH = m_pixels.h / m_scale;
        double const fit = std::min({1.0, vw / naturalW, vh / naturalH});
        m_fitted = {static_cast<float>(naturalW * fit), static_cast<float>(naturalH * fit)};
        m_image.Width(m_fitted.Width);
        m_image.Height(m_fitted.Height);

        double const nativeOverDisplayed = m_pixels.w / (m_fitted.Width * m_scale);
        m_scroll.MaxZoomFactor(static_cast<float>(std::max(1.0, kMaxZoomOverFit * nativeOverDisplayed)));
    }

    void ZoomPanView::ZoomTo(double factor)
    {
        double const current = ZoomFactor();
        double const target = std::clamp(factor, static_cast<double>(m_scroll.MinZoomFactor()), static_cast<double>(m_scroll.MaxZoomFactor()));
        m_targetZoom = target;
        if (std::abs(target - current) < 1e-6)
        {
            return;
        }
        // Keep the viewport centre fixed.
        double const vw = m_scroll.ViewportWidth();
        double const vh = m_scroll.ViewportHeight();
        double const centerX = (m_scroll.HorizontalOffset() + vw / 2) / current;
        double const centerY = (m_scroll.VerticalOffset() + vh / 2) / current;
        m_scroll.ChangeView(Box(centerX * target - vw / 2), Box(centerY * target - vh / 2),
                            winrt::box_value(static_cast<float>(target)).as<Wf::IReference<float>>(), true);
    }

    void ZoomPanView::OnViewChanged(bool intermediate)
    {
        if (!intermediate)
        {
            m_targetZoom = ZoomFactor();
        }
        if (!intermediate && m_timer)
        {
            m_timer.Start(); // restarts the countdown
        }
    }
}
