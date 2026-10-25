#pragma once

#include <windows.h>

#include <functional>

namespace fastlook::daemon
{
    // Watches the foreground window (SetWinEventHook, out of context: callbacks run on the installing thread, i.e. the
    // main thread, during its message loop) and reports when an Explorer window or the desktop became foreground.
    class Prewarm
    {
    public:
        // onCandidate(isDesktop) runs on the main thread and must not block.
        explicit Prewarm(std::function<void(bool isDesktop)> onCandidate);
        ~Prewarm();
        Prewarm(Prewarm const&) = delete;
        Prewarm& operator=(Prewarm const&) = delete;

        void Start(HWND mainWnd);
        void Stop();

    private:
        static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND wnd, LONG idObject, LONG idChild, DWORD thread,
                                          DWORD time);

        std::function<void(bool)> m_onCandidate;
        HWINEVENTHOOK m_hook = nullptr;
    };
}
