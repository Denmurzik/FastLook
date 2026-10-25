#include "pch.h"
#include "Previewers/Image/ImagePreviewer.h"

#include "Diagnostics/Log.h"
#include "Diagnostics/PerfLog.h"
#include "Imaging/DecodeMath.h"
#include "Previewers/Factories.h"
#include "Util/Async.h"

namespace fastlook::viewer
{
    namespace
    {
        using winrt::Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource;

        uint64_t Area(core::SizeU size)
        {
            return static_cast<uint64_t>(size.w) * size.h;
        }
    }

    winrt::Windows::Foundation::IAsyncAction ImagePreviewer::PrepareAsync(PreviewItem item, std::shared_ptr<PreviewContext const> ctx)
    {
        auto self = shared_from_this();
        auto cancel = co_await winrt::get_cancellation_token();
        std::stop_source stop;
        cancel.callback([stop]() mutable { stop.request_stop(); });

        m_path = item.ref.path;
        m_ctx = ctx;
        auto const ui = ctx->ui;
        core::SizeU const target{static_cast<uint32_t>(std::ceil(ctx->maxContentDip.Width * ctx->scale)),
                                 static_cast<uint32_t>(std::ceil(ctx->maxContentDip.Height * ctx->scale))};

        auto wic = WicFactory(); // taken on the UI thread, see DecodeToSize
        co_await winrt::resume_background();
        DecodedImage decoded = DecodeToSize(m_path, target, stop.get_token(), std::move(wic));
        if (cancel() || !decoded.bitmap)
        {
            co_return;
        }

        if (!co_await ResumeOn(ui))
        {
            co_return;
        }
        m_native = core::OrientedSize(decoded.native, decoded.orientation);
        m_preferred = {static_cast<float>(m_native.w / ctx->scale), static_cast<float>(m_native.h / ctx->scale)};
        SoftwareBitmapSource source;
        co_await source.SetBitmapAsync(decoded.bitmap);
        if (cancel())
        {
            source.Close();
            co_return;
        }
        m_baseSource = source;
        m_baseSize = {static_cast<uint32_t>(decoded.bitmap.PixelWidth()), static_cast<uint32_t>(decoded.bitmap.PixelHeight())};
    }

    winrt::Microsoft::UI::Xaml::UIElement ImagePreviewer::View()
    {
        m_zoomView = std::make_unique<ZoomPanView>();
        m_zoomView->SetSource(m_baseSource, m_native, m_ctx->scale);
        m_zoomView->zoomSettled = [this](double zoom) { OnZoomSettled(zoom); };
        return m_zoomView->Root();
    }

    void ImagePreviewer::Zoom(ZoomCommand command)
    {
        if (m_zoomView)
        {
            m_zoomView->Zoom(command);
        }
    }

    void ImagePreviewer::Unload()
    {
        m_redecodeStop.request_stop();
        ++m_generation;
        m_zoomView.reset();
        CloseZoomSource();
        if (m_baseSource)
        {
            try
            {
                m_baseSource.Close();
            }
            catch (...)
            {
            }
            m_baseSource = nullptr;
        }
    }

    void ImagePreviewer::OnZoomSettled(double zoom)
    {
        m_redecodeStop.request_stop();
        m_redecodeStop = {};
        uint64_t const generation = ++m_generation;

        if (zoom <= 1.001)
        {
            m_zoomView->ReplaceSource(m_baseSource);
            CloseZoomSource();
            return;
        }

        // Sharp bitmap for the zoomed view: displayed size x zoom, but at most twice the viewport in each dimension.
        auto const fitted = m_zoomView->FittedDip();
        auto const viewport = m_zoomView->ViewportDip();
        double const scale = m_ctx->scale;
        core::SizeU const wanted = core::ComputeDecodeSize(m_native, {fitted.Width * zoom, fitted.Height * zoom}, scale);
        core::SizeU const cap = core::ComputeDecodeSize(m_native, {viewport.Width * 2.0, viewport.Height * 2.0}, scale);
        core::SizeU const target = Area(wanted) <= Area(cap) ? wanted : cap;
        if (Area(target) <= Area(m_baseSize))
        {
            m_zoomView->ReplaceSource(m_baseSource); // the base bitmap is already at least that sharp
            CloseZoomSource();
            return;
        }
        Redecode(target, m_redecodeStop.get_token(), generation);
    }

    void ImagePreviewer::CloseZoomSource()
    {
        if (auto source = std::exchange(m_zoomSource, nullptr))
        {
            try
            {
                source.Close();
            }
            catch (...)
            {
            }
        }
    }

    winrt::fire_and_forget ImagePreviewer::Redecode(core::SizeU target, std::stop_token stop, uint64_t generation)
    {
        auto self = std::static_pointer_cast<ImagePreviewer>(shared_from_this());
        auto const path = m_path;
        auto const ui = m_ctx->ui;
        auto wic = WicFactory(); // Redecode is called on the UI thread; see DecodeToSize
        try
        {
            core::perf::Mark("redecode", "w=" + std::to_string(target.w) + " h=" + std::to_string(target.h));
            co_await winrt::resume_background();
            DecodedImage decoded = DecodeToSize(path, target, stop, std::move(wic));
            if (stop.stop_requested() || !decoded.bitmap)
            {
                co_return;
            }
            if (!co_await ResumeOn(ui))
            {
                co_return;
            }
            if (generation != self->m_generation || !self->m_zoomView)
            {
                co_return;
            }
            SoftwareBitmapSource source;
            co_await source.SetBitmapAsync(decoded.bitmap);
            if (generation != self->m_generation || !self->m_zoomView)
            {
                source.Close();
                co_return;
            }
            self->m_zoomView->ReplaceSource(source);
            self->CloseZoomSource();
            self->m_zoomSource = source;
        }
        catch (...)
        {
            FL_LOG_INFO(L"Re-decoding the image at the zoomed size failed");
        }
    }

    std::shared_ptr<IPreviewer> CreateImagePreviewer()
    {
        return std::make_shared<ImagePreviewer>();
    }
}
