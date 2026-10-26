#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "Geometry.h"
#include "Imaging/FrameCompositor.h"

namespace fastlook::core
{
    // Upper bound for the colour chunks (PLTE, tRNS, gAMA, cHRM, sRGB, sBIT, iCCP) every frame needs; a file over it is not animated.
    constexpr size_t kMaxApngSharedBytes = 1024 * 1024;

    struct ApngFrame
    {
        FrameInfo info;
        std::vector<uint8_t> idat; // the frame's zlib data (its IDAT or fdAT payloads, concatenated)
    };

    struct ApngFile
    {
        SizeU canvas;
        uint32_t loops = 0; // 0 = forever
        std::array<uint8_t, 13> ihdr{};
        std::vector<uint8_t> shared; // the colour chunks before the image data, as chunks; held once, spliced into each frame
        std::vector<ApngFrame> frames;
    };

    // nullopt for a plain (non-animated) PNG, a canvas over the animation limit (CanAnimateCanvas, checked at IHDR) or a stream
    // that is not a valid APNG. The default image that precedes the first fcTL (when it is not part of the animation) is skipped.
    // Delays are as stored (not normalised). Holds at most the file's image data plus kMaxApngSharedBytes; text, metadata and
    // private chunks are dropped.
    std::optional<ApngFile> ParseApng(std::span<const uint8_t> file);

    // Standalone PNG stream for frame `index`: IHDR with the frame size, the shared colour chunks, IDAT, IEND. Empty when out of range.
    std::vector<uint8_t> BuildApngFramePng(ApngFile const& file, size_t index);

    // Cheap check without copying anything: an animatable IHDR followed by acTL before the first IDAT.
    bool DeclaresApngAnimation(std::span<const uint8_t> file);
}
