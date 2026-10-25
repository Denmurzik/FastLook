#include "Prewarm.h"

#include <cwchar>
#include <iterator>

#include "Diagnostics/Log.h"

#pragma comment(lib, "user32.lib")

namespace fastlook::daemon
{
    namespace
    {
        // One instance per process; the WinEvent callback has no user-data parameter.
        Prewarm* g_instance = nullptr;
    }

    Prewarm::Prewarm(std::function<void(bool)> onCandidate) : m_onCandidate(std::move(onCandidate)) {}

    Prewarm::~Prewarm()
    {
        Stop();
    }

    void Prewarm::Start(HWND)
    {
        if (m_hook)
        {
            return;
        }
        g_instance = this;
        m_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &Prewarm::WinEventProc, 0, 0,
                                 WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (!m_hook)
        {
            FL_LOG_INFO(L"prewarm hook failed {}", GetLastError());
        }
    }

    void Prewarm::Stop()
    {
        if (m_hook)
        {
            UnhookWinEvent(m_hook);
            m_hook = nullptr;
        }
        if (g_instance == this)
        {
            g_instance = nullptr;
        }
    }

    void CALLBACK Prewarm::WinEventProc(HWINEVENTHOOK, DWORD event, HWND wnd, LONG idObject, LONG, DWORD, DWORD)
    {
        if (event != EVENT_SYSTEM_FOREGROUND || idObject != OBJID_WINDOW || !wnd || !g_instance || !g_instance->m_onCandidate)
        {
            return;
        }
        wchar_t cls[64];
        if (GetClassNameW(wnd, cls, static_cast<int>(std::size(cls))) <= 0)
        {
            return;
        }
        if (wcscmp(cls, L"CabinetWClass") == 0)
        {
            g_instance->m_onCandidate(false);
        }
        else if ((wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0) &&
                 FindWindowExW(wnd, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr)
        {
            g_instance->m_onCandidate(true);
        }
    }
}
