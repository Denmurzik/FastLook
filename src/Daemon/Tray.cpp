#include "Tray.h"

#include <algorithm>
#include <string>

#include "Diagnostics/Log.h"
#include "Strings.h"

namespace fastlook::daemon
{
    namespace
    {
        constexpr UINT kTrayId = 1;
        constexpr WORD kIconResource = 1;

        template <size_t N>
        void CopyText(wchar_t (&dst)[N], std::wstring_view src)
        {
            size_t const n = (std::min)(src.size(), N - 1);
            src.copy(dst, n);
            dst[n] = L'\0';
        }
    }

    void Tray::Fill(NOTIFYICONDATAW& nid) const
    {
        nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = m_wnd;
        nid.uID = kTrayId;
    }

    bool Tray::Add(HWND wnd, UINT callbackMsg)
    {
        m_wnd = wnd;

        NOTIFYICONDATAW nid;
        Fill(nid);
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = callbackMsg;
        nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kIconResource));
        CopyText(nid.szTip, S(m_paused ? StrId::TooltipPaused : StrId::Tooltip));

        // A stale icon may still be registered after an Explorer restart; replace it.
        Shell_NotifyIconW(NIM_DELETE, &nid);
        m_added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
        if (m_added)
        {
            FL_LOG_INFO(L"tray added");
        }
        else
        {
            FL_LOG_INFO(L"tray add failed");
        }
        return m_added;
    }

    void Tray::Remove()
    {
        if (!m_added)
        {
            return;
        }
        NOTIFYICONDATAW nid;
        Fill(nid);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        m_added = false;
    }

    void Tray::SetPaused(bool paused)
    {
        m_paused = paused;
        if (!m_added)
        {
            return;
        }
        NOTIFYICONDATAW nid;
        Fill(nid);
        nid.uFlags = NIF_TIP;
        CopyText(nid.szTip, S(paused ? StrId::TooltipPaused : StrId::Tooltip));
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void Tray::ShowBalloon(std::wstring_view title, std::wstring_view text)
    {
        if (!m_added)
        {
            return;
        }
        NOTIFYICONDATAW nid;
        Fill(nid);
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_INFO;
        CopyText(nid.szInfoTitle, title);
        CopyText(nid.szInfo, text);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void Tray::ShowMenu(HWND wnd)
    {
        HMENU menu = CreatePopupMenu();
        if (!menu)
        {
            return;
        }
        std::wstring const settings(S(StrId::TraySettings));
        std::wstring const pause(S(m_paused ? StrId::TrayResume : StrId::TrayPause));
        std::wstring const exit(S(StrId::TrayExit));
        AppendMenuW(menu, MF_STRING, kCmdSettings, settings.c_str());
        AppendMenuW(menu, MF_STRING, kCmdPause, pause.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCmdExit, exit.c_str());

        POINT pt{};
        GetCursorPos(&pt);
        // Without a foreground window the menu would not dismiss on an outside click.
        SetForegroundWindow(wnd);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, wnd, nullptr);
        PostMessageW(wnd, WM_NULL, 0, 0);
        DestroyMenu(menu);
    }
}
