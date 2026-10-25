#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "Geometry.h"

namespace fastlook::core
{
    // Size to decode a `native` image at so it fits into maxDip * scale pixels: aspect kept, never upscaled, at least 1x1.
    SizeU ComputeDecodeSize(SizeU native, SizeD maxDip, double scale);

    // Size after applying EXIF orientation 1..8 (5..8 swap width and height; anything else is treated as 1).
    SizeU OrientedSize(SizeU native, uint16_t exifOrientation);

    // EXIF orientation as "rotate clockwise by N degrees first, then mirror horizontally".
    int ExifRotationDegrees(uint16_t exifOrientation);
    bool ExifFlipsHorizontally(uint16_t exifOrientation);

    // Browser behaviour: a frame delay below 20 ms (including 0) plays as 100 ms.
    uint32_t NormalizeFrameDelayMs(uint32_t delayMs);

    // Lead ruling (overrides decision 52's 4096x4096): animate only canvases up to 3 000 000 pixels (covers 1920x1080 and 1600x1600); larger ones
    // show their first frame through the still path. Checked on header values, before anything is allocated for the canvas.
    constexpr uint64_t kMaxAnimatedPixels = 3'000'000;
    bool CanAnimateCanvas(SizeU canvas);

    // Logical screen size from the first 10 bytes of a GIF ("GIF87a"/"GIF89a"); nullopt for anything else.
    std::optional<SizeU> GifLogicalScreenSize(std::span<const uint8_t> file);

    // SVG length attribute ("24", "24px", "2in", "10mm", "12pt", ...) in CSS px; nullopt for percentages, relative units and garbage.
    std::optional<double> ParseSvgLength(std::wstring_view text);

    // Intrinsic size of an SVG in CSS px from the root's width/height (percentages count as absent) and viewBox: both given -> as is;
    // one given -> the other from the viewBox aspect (square without one); none -> the viewBox size, else 512x512. Clamped to 16384.
    SizeD SvgIntrinsicSize(std::optional<double> width, std::optional<double> height, std::optional<SizeD> viewBox);
}
