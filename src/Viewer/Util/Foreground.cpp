#include "pch.h"
#include "Util/Foreground.h"

#include "Hook/HookFilter.h"

namespace fastlook::viewer
{
    void ForceForeground(HWND hwnd)
    {
        if (hwnd == nullptr)
        {
            return;
        }
        INPUT inputs[2]{};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = 0xE8;
        inputs[0].ki.dwExtraInfo = core::hook::kInjectedMarker;
        inputs[1] = inputs[0];
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, inputs, sizeof(INPUT));
        SetForegroundWindow(hwnd);
    }
}
