#pragma once

#include <cstdint>
#include <filesystem>
#include <stop_token>

#include <winrt/Windows.Graphics.Imaging.h>

#include <wincodec.h>

#include "Geometry.h"

namespace fastlook::viewer
{
    struct DecodedImage
    {
        winrt::Windows::Graphics::Imaging::SoftwareBitmap bitmap{nullptr}; // oriented, Bgra8 premultiplied; null when cancelled
        core::SizeU native;                                                // pixels as stored, before orientation
        uint16_t orientation = 1;
        uint32_t frameCount = 1;
    };

    // Decodes straight to a bitmap that fits into `target` pixels (aspect kept, never upscaled), applying the EXIF orientation.
    // `target` is in oriented space. ICO files use their largest frame. Throws PreviewError (no codec / decode failed).
    // Blocking; call on a background thread. A requested stop returns an image with a null bitmap.
    // `wic` must be a reference taken on the UI thread (`WicFactory()`) before going to the background: DeviceResources::ReleaseAll
    // runs on the UI thread, so a reference acquired there can never race with it and keeps the factory alive for the decode.
    DecodedImage DecodeToSize(std::filesystem::path const& path, core::SizeU target, std::stop_token stop,
                              winrt::com_ptr<IWICImagingFactory2> wic);

    // UI thread only. Null when the factory cannot be created.
    winrt::com_ptr<IWICImagingFactory2> WicFactory();
}
