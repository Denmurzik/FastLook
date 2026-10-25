#include "Imaging/DecodeMath.h"

#include <algorithm>
#include <cmath>

namespace fastlook::core
{
    namespace
    {
        constexpr int kRotation[] = {0, 0, 0, 180, 180, 90, 90, 270, 270};                      // index = orientation 1..8
        constexpr bool kFlip[] = {false, false, true, false, true, true, false, true, false};

        bool Valid(uint16_t orientation)
        {
            return orientation >= 1 && orientation <= 8;
        }

        uint32_t Clamp(double value, uint32_t limit)
        {
            return static_cast<uint32_t>(std::clamp(std::llround(value), 1LL, static_cast<long long>(std::max(limit, 1u))));
        }
    }

    SizeU ComputeDecodeSize(SizeU native, SizeD maxDip, double scale)
    {
        if (native.w == 0 || native.h == 0)
        {
            return {1, 1};
        }
        double const maxW = maxDip.w * scale;
        double const maxH = maxDip.h * scale;
        double const ratio = std::min({1.0, maxW / native.w, maxH / native.h});
        if (!(ratio > 0.0))
        {
            return {1, 1};
        }
        if (ratio >= 1.0)
        {
            return native;
        }
        return {Clamp(native.w * ratio, native.w), Clamp(native.h * ratio, native.h)};
    }

    SizeU OrientedSize(SizeU native, uint16_t exifOrientation)
    {
        return Valid(exifOrientation) && exifOrientation >= 5 ? SizeU{native.h, native.w} : native;
    }

    int ExifRotationDegrees(uint16_t exifOrientation)
    {
        return Valid(exifOrientation) ? kRotation[exifOrientation] : 0;
    }

    bool ExifFlipsHorizontally(uint16_t exifOrientation)
    {
        return Valid(exifOrientation) && kFlip[exifOrientation];
    }
}
