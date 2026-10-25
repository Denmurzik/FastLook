#pragma once

#include <windows.h>
#include <shellapi.h>

#include <string_view>

namespace fastlook::daemon
{
    enum TrayCommand : UINT
    {
        kCmdSettings = 1001,
        kCmdPause = 1002,
        kCmdExit = 1003
    };

    // Notification-area icon. The callback message's lParam is the raw mouse message (legacy NOTIFYICON behaviour).
    class Tray
    {
    public:
        // NIM_ADD; also used to re-add the icon after Explorer restarts (TaskbarCreated).
        bool Add(HWND wnd, UINT callbackMsg);
        void Remove();
        void SetPaused(bool paused);
        void ShowBalloon(std::wstring_view title, std::wstring_view text);
        // Menu choices arrive as WM_COMMAND (TrayCommand) on `wnd`.
        void ShowMenu(HWND wnd);

    private:
        void Fill(NOTIFYICONDATAW& nid) const;

        HWND m_wnd = nullptr;
        bool m_added = false;
        bool m_paused = false;
    };
}
