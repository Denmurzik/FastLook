#pragma once

#include <windows.h>

#include <string_view>

#include "Hook/HookFilter.h"

namespace fastlook::daemon
{
    // Stub until the keyboard hook thread lands (Task 27): logs only.
    class HookThread
    {
    public:
        bool Start(HWND notify, core::hook::HookConfig config);
        void Stop();
        void SetConfig(core::hook::HookConfig config);
        void Rehook(std::string_view reason);
        void CheckWatchdog();

    private:
        core::hook::HookConfig m_config;
    };
}
