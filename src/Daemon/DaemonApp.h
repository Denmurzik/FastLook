#pragma once

#include <windows.h>

#include <string>

#include "Cli/DaemonArgs.h"
#include "Hook/HookThread.h"
#include "PreviewController.h"
#include "Settings/Settings.h"
#include "Tray.h"

namespace fastlook::daemon
{
    // Hidden top-level window (WS_POPUP, WS_EX_TOOLWINDOW, never shown) - NOT HWND_MESSAGE: it must receive the
    // TaskbarCreated and WM_POWERBROADCAST broadcasts.
    inline constexpr wchar_t kWindowClass[] = L"FastLookDaemonWnd";
    inline constexpr wchar_t kMutexName[] = L"Local\\FastLook-Daemon";

    enum : UINT
    {
        WM_FL_TRIGGER = WM_APP + 1, // wParam = foreground HWND
        WM_FL_TRAY = WM_APP + 2,
        WM_FL_SETTINGS_CHANGED = WM_APP + 3
    };

    enum CopyDataKind : ULONG_PTR
    {
        kCopyOpenSettings = 1,
        kCopyShow = 2, // UTF-16 path
        kCopyShutdown = 3,
        kCopyToggle = 4 // UTF-16 path
    };

    class DaemonApp
    {
    public:
        // The first (and only) daemon instance: creates the window, tray and notifications, runs the message loop.
        // Returns the process exit code.
        int Run(core::DaemonArgs const& args);

    private:
        static LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT OnMessage(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam);
        bool OnCopyData(COPYDATASTRUCT const& cds);
        void OnCommand(UINT id);
        void ReloadSettings();
        core::hook::HookConfig MakeHookConfig() const;
        void Exit();

        HWND m_wnd = nullptr;
        UINT m_taskbarCreated = 0;
        bool m_paused = false;
        bool m_exiting = false;
        core::Settings m_settings;
        Tray m_tray;
        HookThread m_hook;
        PreviewController m_controller;
    };
}
