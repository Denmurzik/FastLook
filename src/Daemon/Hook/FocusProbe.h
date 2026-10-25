#pragma once

#include <windows.h>

#include "Hook/HookFilter.h"

namespace fastlook::daemon
{
    // Both run inside the low-level keyboard hook: no allocation, no locks, no COM.
    core::hook::ForegroundKind ClassifyForeground(HWND fg) noexcept;
    void ProbeFocus(HWND fg, core::hook::FocusSnapshot& out) noexcept;
}
