#include "HookThread.h"

#include "Diagnostics/Log.h"
#include "Text/Utf.h"

namespace fastlook::daemon
{
    bool HookThread::Start(HWND, core::hook::HookConfig config)
    {
        m_config = config;
        FL_LOG_INFO(L"hook start (stub)");
        return true;
    }

    void HookThread::Stop()
    {
        FL_LOG_INFO(L"hook stop (stub)");
    }

    void HookThread::SetConfig(core::hook::HookConfig config)
    {
        m_config = config;
        FL_LOG_INFO(L"hook config paused={}", config.paused);
    }

    void HookThread::Rehook(std::string_view reason)
    {
        FL_LOG_INFO(L"rehook reason={}", core::FromUtf8(reason));
    }

    void HookThread::CheckWatchdog()
    {
    }
}
