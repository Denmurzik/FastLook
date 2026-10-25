#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

#include <wincodec.h>

#include "Geometry.h"

namespace fastlook::viewer
{
    // Animated GIF, APNG and WebP. Frames are decoded lazily: frame N+1 is decoded on a background thread while frame N is
    // shown, and a DispatcherQueueTimer per frame delay swaps it into one WriteableBitmap. Loops as stored in the file.
    class AnimatedImage : public std::enable_shared_from_this<AnimatedImage>
    {
    public:
        struct FrameSource;

        // Background thread. Null when the file is not animated (one frame, wrong container), is larger than 4096x4096 or cannot
        // be read: the caller falls back to the still image path, which also reports the error. `wic` must come from `WicFactory()`
        // on the UI thread. Decodes the first frame.
        static std::shared_ptr<AnimatedImage> Open(std::filesystem::path const& path, winrt::com_ptr<IWICImagingFactory2> wic, std::stop_token stop);

        AnimatedImage(std::unique_ptr<FrameSource> source, core::SizeU canvas);
        ~AnimatedImage();

        core::SizeU Canvas() const { return m_canvas; }

        // UI thread: creates the bitmap, shows the first frame and starts playing. The returned bitmap lives until Stop().
        winrt::Microsoft::UI::Xaml::Media::ImageSource Attach(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& ui);
        // UI thread, idempotent: stops the timer and any further decoding.
        void Stop();

    private:
        void Present();
        void TryPresent();
        winrt::fire_and_forget DecodeNext();

        std::unique_ptr<FrameSource> m_source; // touched by one background decode at a time
        core::SizeU m_canvas{};
        std::vector<uint32_t> m_pending; // next frame, written by the background decode, read on the UI thread afterwards
        uint32_t m_pendingDelayMs = 100;

        std::atomic<bool> m_stopped{false};
        winrt::Microsoft::UI::Dispatching::DispatcherQueue m_ui{nullptr};
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_timer{nullptr};
        winrt::event_token m_tick{};
        winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap m_bitmap{nullptr};
        uint64_t m_presented = 0;
        bool m_timerElapsed = false;
        bool m_decodeReady = false;
    };
}
