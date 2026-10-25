#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "Text/TokenCategory.h"

namespace fastlook::viewer
{
    struct StyledRun
    {
        uint32_t start = 0, length = 0; // UTF-16 units within the line
        core::TokenCategory category = core::TokenCategory::Default;
    };

    struct ILineStyler
    {
        virtual ~ILineStyler() = default;
        virtual void StyleLine(uint64_t line, std::wstring_view text, std::vector<StyledRun>& out) = 0; // UI thread, must be cheap
        virtual void EnsureStyledThrough(uint64_t lastVisibleLine) = 0;                                 // schedules background work
        std::function<void()> invalidated;                                                              // styler -> CodeView redraw
    };
}
