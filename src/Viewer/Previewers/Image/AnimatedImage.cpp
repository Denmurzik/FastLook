#include "pch.h"
#include "Previewers/Image/AnimatedImage.h"

#include <robuffer.h>

#include <webp/decode.h>
#include <webp/demux.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <optional>
#include <span>
#include <stdexcept>

#include "Diagnostics/Log.h"
#include "Graphics/DeviceResources.h"
#include "Imaging/Apng.h"
#include "Imaging/DecodeMath.h"
#include "Imaging/FrameCompositor.h"
#include "Util/Async.h"

namespace fastlook::viewer
{
    struct AnimatedImage::FrameSource
    {
        virtual ~FrameSource() = default;
        virtual core::SizeU Canvas() const = 0;
        virtual uint32_t FrameCount() const = 0; // 0 when unknown
        virtual uint32_t Loops() const = 0;      // 0 = forever
        // Decodes the next frame as a full canvas (premultiplied BGRA) into `out` and returns its stored delay in ms.
        // Wraps to the first frame after the last one. Throws on a decode failure.
        virtual uint32_t Next(std::span<uint32_t> out) = 0;
    };

    namespace
    {
        using winrt::check_hresult;
        using winrt::com_ptr;

        constexpr uintmax_t kMaxFileBytes = 64ull * 1024 * 1024; // the whole file is held in memory so that it is never locked

        std::wstring Lower(std::filesystem::path const& path)
        {
            auto ext = path.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            return ext;
        }

        std::vector<uint8_t> ReadWholeFile(std::filesystem::path const& path)
        {
            winrt::file_handle file{::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
            LARGE_INTEGER size{};
            if (!file || !::GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 || static_cast<uintmax_t>(size.QuadPart) > kMaxFileBytes)
            {
                return {};
            }
            std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            if (!::ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size())
            {
                return {};
            }
            return bytes;
        }

        std::optional<uint32_t> ReadMeta(IWICMetadataQueryReader* reader, wchar_t const* query)
        {
            if (reader == nullptr)
            {
                return std::nullopt;
            }
            PROPVARIANT value;
            PropVariantInit(&value);
            std::optional<uint32_t> result;
            if (SUCCEEDED(reader->GetMetadataByName(query, &value)))
            {
                switch (value.vt)
                {
                case VT_UI1: result = value.bVal; break;
                case VT_UI2: result = value.uiVal; break;
                case VT_UI4: result = value.ulVal; break;
                default: break;
                }
            }
            PropVariantClear(&value);
            return result;
        }

        com_ptr<IWICBitmapDecoder> DecoderFromMemory(IWICImagingFactory2* wic, std::span<const uint8_t> bytes)
        {
            com_ptr<IWICStream> stream;
            check_hresult(wic->CreateStream(stream.put()));
            check_hresult(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size())));
            com_ptr<IWICBitmapDecoder> decoder;
            check_hresult(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put()));
            return decoder; // the decoder keeps the stream alive; `bytes` must outlive the decoder
        }

        // Converts a decoded frame to premultiplied BGRA and copies it into `out` (w * h pixels). Returns the frame size.
        // A frame larger than the canvas is rejected before anything is allocated for it.
        core::SizeU CopyFramePbgra(IWICImagingFactory2* wic, IWICBitmapFrameDecode* frame, core::SizeU canvas, std::vector<uint32_t>& out)
        {
            UINT frameW = 0, frameH = 0;
            check_hresult(frame->GetSize(&frameW, &frameH));
            if (frameW == 0 || frameH == 0 || frameW > canvas.w || frameH > canvas.h)
            {
                throw std::runtime_error("animation frame does not fit the canvas");
            }
            com_ptr<IWICPalette> palette;
            if (SUCCEEDED(wic->CreatePalette(palette.put())) && FAILED(frame->CopyPalette(palette.get())))
            {
                palette = nullptr;
            }
            com_ptr<IWICFormatConverter> converter;
            check_hresult(wic->CreateFormatConverter(converter.put()));
            check_hresult(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, palette.get(), 0.0,
                                                WICBitmapPaletteTypeCustom));
            UINT w = 0, h = 0;
            check_hresult(converter->GetSize(&w, &h));
            out.resize(static_cast<size_t>(w) * h);
            check_hresult(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.size() * 4), reinterpret_cast<BYTE*>(out.data())));
            return {w, h};
        }

        class GifSource final : public AnimatedImage::FrameSource
        {
        public:
            // `canvas` is the logical screen from the file header, already checked against the animation size limit.
            GifSource(com_ptr<IWICImagingFactory2> wic, std::vector<uint8_t> bytes, core::SizeU canvas)
                : m_wic(std::move(wic)), m_bytes(std::move(bytes)), m_canvas(canvas)
            {
                m_decoder = DecoderFromMemory(m_wic.get(), m_bytes);
                check_hresult(m_decoder->GetFrameCount(&m_count));
                m_compositor.Begin(m_canvas);
            }

            core::SizeU Canvas() const override { return m_canvas; }
            uint32_t FrameCount() const override { return m_count; }
            uint32_t Loops() const override { return 0; }

            uint32_t Next(std::span<uint32_t> out) override
            {
                com_ptr<IWICBitmapFrameDecode> frame;
                check_hresult(m_decoder->GetFrame(m_index, frame.put()));
                com_ptr<IWICMetadataQueryReader> meta;
                if (FAILED(frame->GetMetadataQueryReader(meta.put())))
                {
                    meta = nullptr;
                }
                core::FrameInfo info;
                info.rect.x = static_cast<int32_t>(ReadMeta(meta.get(), L"/imgdesc/Left").value_or(0));
                info.rect.y = static_cast<int32_t>(ReadMeta(meta.get(), L"/imgdesc/Top").value_or(0));
                info.delayMs = ReadMeta(meta.get(), L"/grctlext/Delay").value_or(0) * 10;
                switch (ReadMeta(meta.get(), L"/grctlext/Disposal").value_or(0))
                {
                case 2: info.dispose = core::Dispose::Background; break;
                case 3: info.dispose = core::Dispose::Previous; break;
                default: info.dispose = core::Dispose::None; break;
                }
                info.blend = core::Blend::Over;

                auto const size = CopyFramePbgra(m_wic.get(), frame.get(), m_canvas, m_frame);
                info.rect.w = static_cast<int32_t>(size.w);
                info.rect.h = static_cast<int32_t>(size.h);
                m_compositor.Apply(info, m_frame);
                std::vector<uint32_t>().swap(m_frame); // not needed until the next frame
                std::copy_n(m_compositor.Canvas().begin(), std::min(out.size(), m_compositor.Canvas().size()), out.begin());

                if (++m_index >= m_count)
                {
                    m_index = 0;
                    m_compositor.Restart();
                }
                return info.delayMs;
            }

        private:
            com_ptr<IWICImagingFactory2> m_wic;
            std::vector<uint8_t> m_bytes;
            com_ptr<IWICBitmapDecoder> m_decoder;
            UINT m_count = 0;
            UINT m_index = 0;
            core::SizeU m_canvas{};
            core::FrameCompositor m_compositor;
            std::vector<uint32_t> m_frame;
        };

        class ApngSource final : public AnimatedImage::FrameSource
        {
        public:
            ApngSource(com_ptr<IWICImagingFactory2> wic, core::ApngFile file) : m_wic(std::move(wic)), m_file(std::move(file))
            {
                m_compositor.Begin(m_file.canvas);
            }

            core::SizeU Canvas() const override { return m_file.canvas; }
            uint32_t FrameCount() const override { return static_cast<uint32_t>(m_file.frames.size()); }
            uint32_t Loops() const override { return m_file.loops; }

            uint32_t Next(std::span<uint32_t> out) override
            {
                auto const& entry = m_file.frames[m_index];
                auto const png = core::BuildApngFramePng(m_file, m_index); // built per frame: the colour chunks are held once
                auto decoder = DecoderFromMemory(m_wic.get(), png);
                com_ptr<IWICBitmapFrameDecode> frame;
                check_hresult(decoder->GetFrame(0, frame.put()));
                auto const size = CopyFramePbgra(m_wic.get(), frame.get(), m_file.canvas, m_frame);
                if (size.w != static_cast<uint32_t>(entry.info.rect.w) || size.h != static_cast<uint32_t>(entry.info.rect.h))
                {
                    throw std::runtime_error("APNG frame size mismatch");
                }
                m_compositor.Apply(entry.info, m_frame);
                std::vector<uint32_t>().swap(m_frame); // not needed until the next frame
                std::copy_n(m_compositor.Canvas().begin(), std::min(out.size(), m_compositor.Canvas().size()), out.begin());

                uint32_t const delay = entry.info.delayMs;
                if (++m_index >= m_file.frames.size())
                {
                    m_index = 0;
                    m_compositor.Restart();
                }
                return delay;
            }

        private:
            com_ptr<IWICImagingFactory2> m_wic;
            core::ApngFile m_file;
            size_t m_index = 0;
            core::FrameCompositor m_compositor;
            std::vector<uint32_t> m_frame;
        };

        // libwebp composites full canvases itself (premultiplied BGRA with MODE_bgrA).
        class WebpSource final : public AnimatedImage::FrameSource
        {
        public:
            explicit WebpSource(std::vector<uint8_t> bytes) : m_bytes(std::move(bytes))
            {
                WebPAnimDecoderOptions options;
                if (!WebPAnimDecoderOptionsInit(&options))
                {
                    throw std::runtime_error("WebP version mismatch");
                }
                options.color_mode = MODE_bgrA;
                options.use_threads = 0;
                WebPData data{m_bytes.data(), m_bytes.size()};
                m_decoder = WebPAnimDecoderNew(&data, &options);
                WebPAnimInfo info{};
                if (m_decoder == nullptr || !WebPAnimDecoderGetInfo(m_decoder, &info))
                {
                    throw std::runtime_error("WebP animation could not be opened");
                }
                m_canvas = {info.canvas_width, info.canvas_height};
                m_count = info.frame_count;
                m_loops = info.loop_count;
            }

            ~WebpSource() override
            {
                if (m_decoder != nullptr)
                {
                    WebPAnimDecoderDelete(m_decoder);
                }
            }

            core::SizeU Canvas() const override { return m_canvas; }
            uint32_t FrameCount() const override { return m_count; }
            uint32_t Loops() const override { return m_loops; }

            uint32_t Next(std::span<uint32_t> out) override
            {
                if (!WebPAnimDecoderHasMoreFrames(m_decoder))
                {
                    WebPAnimDecoderReset(m_decoder);
                    m_lastTimestamp = 0;
                }
                uint8_t* pixels = nullptr;
                int timestamp = 0;
                if (!WebPAnimDecoderGetNext(m_decoder, &pixels, &timestamp))
                {
                    throw std::runtime_error("WebP frame could not be decoded");
                }
                std::memcpy(out.data(), pixels, std::min(out.size() * sizeof(uint32_t), static_cast<size_t>(m_canvas.w) * m_canvas.h * 4));
                uint32_t const delay = static_cast<uint32_t>(std::max(0, timestamp - m_lastTimestamp));
                m_lastTimestamp = timestamp;
                return delay;
            }

        private:
            std::vector<uint8_t> m_bytes;
            WebPAnimDecoder* m_decoder = nullptr;
            core::SizeU m_canvas{};
            uint32_t m_count = 0;
            uint32_t m_loops = 0;
            int m_lastTimestamp = 0;
        };

        std::unique_ptr<AnimatedImage::FrameSource> CreateSource(std::filesystem::path const& path, com_ptr<IWICImagingFactory2> const& wic)
        {
            auto const ext = Lower(path);
            if (ext == L".gif")
            {
                auto bytes = ReadWholeFile(path);
                if (bytes.empty() || !wic)
                {
                    return nullptr;
                }
                // Header values are untrusted: the size limit is checked before any decoder or canvas exists.
                auto const screen = core::GifLogicalScreenSize(bytes);
                if (!screen || !core::CanAnimateCanvas(*screen))
                {
                    return nullptr;
                }
                auto source = std::make_unique<GifSource>(wic, std::move(bytes), *screen);
                return source->FrameCount() > 1 ? std::move(source) : nullptr;
            }
            if (ext == L".png" || ext == L".apng")
            {
                auto bytes = ReadWholeFile(path);
                if (bytes.empty() || !wic)
                {
                    return nullptr;
                }
                auto file = core::ParseApng(bytes);
                if (!file || file->frames.size() < 2 || !core::CanAnimateCanvas(file->canvas))
                {
                    return nullptr;
                }
                bytes = {};
                return std::make_unique<ApngSource>(wic, std::move(*file));
            }
            if (ext == L".webp")
            {
                auto bytes = ReadWholeFile(path);
                WebPBitstreamFeatures features{};
                if (bytes.empty() || WebPGetFeatures(bytes.data(), bytes.size(), &features) != VP8_STATUS_OK || !features.has_animation ||
                    !core::CanAnimateCanvas({static_cast<uint32_t>(std::max(features.width, 0)), static_cast<uint32_t>(std::max(features.height, 0))}))
                {
                    return nullptr;
                }
                auto source = std::make_unique<WebpSource>(std::move(bytes));
                return source->FrameCount() > 1 ? std::move(source) : nullptr;
            }
            return nullptr;
        }

        // The same header checks CreateSource makes, without parsing frames or building a source.
        bool LooksAnimated(std::filesystem::path const& path, com_ptr<IWICImagingFactory2> const& wic)
        {
            auto const ext = Lower(path);
            if (ext != L".gif" && ext != L".png" && ext != L".apng" && ext != L".webp")
            {
                return false;
            }
            auto const bytes = ReadWholeFile(path);
            if (bytes.empty())
            {
                return false;
            }
            if (ext == L".png" || ext == L".apng")
            {
                return core::DeclaresApngAnimation(bytes);
            }
            if (ext == L".webp")
            {
                WebPBitstreamFeatures features{};
                return WebPGetFeatures(bytes.data(), bytes.size(), &features) == VP8_STATUS_OK && features.has_animation;
            }
            auto const screen = core::GifLogicalScreenSize(bytes);
            if (!screen || !core::CanAnimateCanvas(*screen) || !wic)
            {
                return false;
            }
            UINT count = 0;
            return SUCCEEDED(DecoderFromMemory(wic.get(), bytes)->GetFrameCount(&count)) && count > 1;
        }

        uint8_t* BufferData(winrt::Windows::Storage::Streams::IBuffer const& buffer)
        {
            uint8_t* data = nullptr;
            check_hresult(buffer.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&data));
            return data;
        }
    }

    std::shared_ptr<AnimatedImage> AnimatedImage::Open(std::filesystem::path const& path, com_ptr<IWICImagingFactory2> wic, std::stop_token stop)
    {
        try
        {
            auto source = CreateSource(path, wic);
            if (!source || !core::CanAnimateCanvas(source->Canvas()) || stop.stop_requested())
            {
                return nullptr; // the still path shows the first frame (and reports an unreadable file)
            }
            auto const canvas = source->Canvas();
            auto image = std::make_shared<AnimatedImage>(std::move(source), canvas);
            image->m_pending.assign(static_cast<size_t>(canvas.w) * canvas.h, 0);
            image->m_pendingDelayMs = image->m_source->Next(image->m_pending);
            return image;
        }
        catch (...)
        {
            FL_LOG_INFO(L"Animated image could not be opened; showing the first frame");
            return nullptr;
        }
    }

    bool AnimatedImage::IsAnimated(std::filesystem::path const& path, com_ptr<IWICImagingFactory2> const& wic)
    {
        try
        {
            return LooksAnimated(path, wic);
        }
        catch (...)
        {
            return false;
        }
    }

    AnimatedImage::AnimatedImage(std::unique_ptr<FrameSource> source, core::SizeU canvas) : m_source(std::move(source)), m_canvas(canvas) {}

    AnimatedImage::~AnimatedImage() = default;

    winrt::Microsoft::UI::Xaml::Media::ImageSource AnimatedImage::Attach(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& ui)
    {
        if (m_bitmap)
        {
            return m_bitmap; // already attached: one timer, one bitmap
        }
        m_ui = ui;
        m_bitmap = winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap{static_cast<int32_t>(m_canvas.w), static_cast<int32_t>(m_canvas.h)};
        m_timer = ui.CreateTimer();
        m_timer.IsRepeating(false);
        m_tick = m_timer.Tick([this](auto const&, auto const&) {
            m_timerElapsed = true;
            TryPresent();
        });
        Present();
        return m_bitmap;
    }

    void AnimatedImage::Stop()
    {
        if (m_stopped.exchange(true))
        {
            return;
        }
        if (m_timer)
        {
            m_timer.Stop();
            m_timer.Tick(m_tick);
            m_timer = nullptr;
        }
        m_bitmap = nullptr;
    }

    void AnimatedImage::Present()
    {
        auto const buffer = m_bitmap.PixelBuffer();
        size_t const bytes = std::min<size_t>(buffer.Length(), m_pending.size() * sizeof(uint32_t));
        std::memcpy(BufferData(buffer), m_pending.data(), bytes);
        m_bitmap.Invalidate();

        ++m_presented;
        m_timerElapsed = false;
        m_decodeReady = false;

        uint32_t const frames = m_source->FrameCount();
        uint32_t const loops = m_source->Loops();
        if (loops != 0 && frames != 0 && m_presented >= static_cast<uint64_t>(loops) * frames)
        {
            return; // the last loop ended: stay on the last frame
        }
        m_timer.Interval(std::chrono::milliseconds(core::NormalizeFrameDelayMs(m_pendingDelayMs)));
        m_timer.Start();
        DecodeNext();
    }

    void AnimatedImage::TryPresent()
    {
        if (!m_stopped && m_timerElapsed && m_decodeReady)
        {
            Present();
        }
    }

    winrt::fire_and_forget AnimatedImage::DecodeNext()
    {
        auto self = shared_from_this();
        auto const ui = m_ui;
        try
        {
            co_await winrt::resume_background();
            if (m_stopped)
            {
                co_return;
            }
            uint32_t const delay = m_source->Next(m_pending);
            if (!co_await ResumeOn(ui))
            {
                co_return;
            }
            if (m_stopped)
            {
                co_return;
            }
            m_pendingDelayMs = delay;
            m_decodeReady = true;
            TryPresent();
        }
        catch (...)
        {
            FL_LOG_INFO(L"Decoding the next animation frame failed; playback stops on the current frame");
        }
    }
}
