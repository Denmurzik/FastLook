#pragma once

#include <cstdint>
#include <span>

namespace fastlook::core
{
    // Heuristic "is this the beginning of a text file" over at most the first 4 KB.
    bool LooksLikeText(std::span<const uint8_t> head);
}
