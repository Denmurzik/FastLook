#pragma once

#include <cstdint>

namespace fastlook::core
{
    enum class TokenCategory : uint8_t
    {
        Default,
        Comment,
        String,
        Number,
        Keyword,
        Preprocessor,
        Operator,
        Identifier,
        Error
    };
}
