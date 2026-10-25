#pragma once

#include <cstdint>

namespace fastlook::core
{
    struct RectI
    {
        int32_t x = 0, y = 0, w = 0, h = 0;
        bool operator==(RectI const&) const = default;
    };

    struct SizeD
    {
        double w = 0, h = 0;
        bool operator==(SizeD const&) const = default;
    };

    struct SizeU
    {
        uint32_t w = 0, h = 0;
        bool operator==(SizeU const&) const = default;
    };
}
