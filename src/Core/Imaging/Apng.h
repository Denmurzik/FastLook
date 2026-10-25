#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "Geometry.h"
#include "Imaging/FrameCompositor.h"

namespace fastlook::core
{
    struct ApngFrame
    {
        FrameInfo info;
        std::vector<uint8_t> png; // standalone PNG stream: IHDR with the frame size + IDAT built from the frame's fdAT/IDAT data
    };

    struct ApngFile
    {
        SizeU canvas;
        uint32_t loops = 0; // 0 = forever
        std::vector<ApngFrame> frames;
    };

    // nullopt for a plain (non-animated) PNG or a stream that is not a valid APNG. The default image that precedes the first
    // fcTL (when it is not part of the animation) is skipped. Delays are as stored (not normalised).
    std::optional<ApngFile> ParseApng(std::span<const uint8_t> file);
}
