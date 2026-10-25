#pragma once

#include <cstdint>
#include <optional>

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

    // Plan decision 52: canvases above 4096x4096 show their first frame only.
    bool CanAnimateCanvas(SizeU canvas);

    // Intrinsic size of an SVG in CSS px from the root's width/height (percentages count as absent) and viewBox: both given -> as is;
    // one given -> the other from the viewBox aspect (square without one); none -> the viewBox size, else 512x512. Clamped to 16384.
    SizeD SvgIntrinsicSize(std::optional<double> width, std::optional<double> height, std::optional<SizeD> viewBox);
}
