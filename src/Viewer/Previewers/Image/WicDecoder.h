#pragma once

#include <cstdint>
#include <filesystem>
#include <stop_token>

#include <winrt/Windows.Graphics.Imaging.h>

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
    DecodedImage DecodeToSize(std::filesystem::path const& path, core::SizeU target, std::stop_token stop);
}
