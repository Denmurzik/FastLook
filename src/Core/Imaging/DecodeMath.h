#pragma once

#include <cstdint>

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
}
