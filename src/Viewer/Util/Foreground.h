#pragma once

#include <windows.h>

namespace fastlook::viewer
{
    // Brings `hwnd` to the foreground from a process that has no recent input: a dummy key press tagged with the hook's
    // injected marker (ignored by the hook) makes SetForegroundWindow honoured (decision 25).
    void ForceForeground(HWND hwnd);
}
