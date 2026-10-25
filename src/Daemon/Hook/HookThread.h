#pragma once

#include <windows.h>

#include <memory>
#include <string_view>

#include "Hook/HookFilter.h"

namespace fastlook::daemon
{
    // Owns the low-level keyboard hook thread. Start/Stop/SetConfig/Rehook/CheckWatchdog are called from the main thread.
    class HookThread
    {
    public:
        struct Context; // defined in HookThread.cpp

        ~HookThread();
        bool Start(HWND notify, core::hook::HookConfig config);
        void Stop();
        void SetConfig(core::hook::HookConfig config);
        void Rehook(std::string_view reason);
        // Recreates the hook thread when its heartbeat is stale.
        void CheckWatchdog();

    private:
        bool Launch();

        HWND m_notify = nullptr;
        core::hook::HookConfig m_config;
        std::shared_ptr<Context> m_ctx;
        HANDLE m_thread = nullptr;
        bool m_stopped = false;
    };
}
