#include "FocusProbe.h"

#include <cwchar>
#include <iterator>

#include "../SharedState.h"

namespace fastlook::daemon
{
    namespace
    {
        constexpr int kMaxAncestorLevels = 8;

        bool ClassIs(HWND wnd, wchar_t const* name) noexcept
        {
            wchar_t buf[64];
            return GetClassNameW(wnd, buf, static_cast<int>(std::size(buf))) > 0 && wcscmp(buf, name) == 0;
        }
    }

    core::hook::ForegroundKind ClassifyForeground(HWND fg) noexcept
    {
        using core::hook::ForegroundKind;
        if (!fg)
        {
            return ForegroundKind::Other;
        }
        wchar_t cls[64];
        if (GetClassNameW(fg, cls, static_cast<int>(std::size(cls))) > 0)
        {
            if (wcscmp(cls, L"CabinetWClass") == 0)
            {
                return ForegroundKind::Explorer;
            }
            if ((wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0) &&
                FindWindowExW(fg, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr)
            {
                return ForegroundKind::Desktop;
            }
        }
        DWORD const viewerPid = g_viewerPid.load(std::memory_order_relaxed);
        if (viewerPid != 0)
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            if (pid == viewerPid)
            {
                return ForegroundKind::OurPreview;
            }
        }
        return ForegroundKind::Other;
    }

    void ProbeFocus(HWND fg, core::hook::FocusSnapshot& out) noexcept
    {
        out = {};
        out.fg = ClassifyForeground(fg);
        if (!fg)
        {
            return;
        }
        DWORD const thread = GetWindowThreadProcessId(fg, nullptr);
        GUITHREADINFO gti{};
        gti.cbSize = sizeof(gti);
        if (thread == 0 || !GetGUIThreadInfo(thread, &gti))
        {
            return;
        }
        out.guiFlags = gti.flags;
        if (gti.hwndFocus)
        {
            GetClassNameW(gti.hwndFocus, out.focusClass, static_cast<int>(std::size(out.focusClass)));
            HWND wnd = gti.hwndFocus;
            for (int i = 0; wnd && i < kMaxAncestorLevels; ++i, wnd = GetParent(wnd))
            {
                if (ClassIs(wnd, L"SHELLDLL_DefView"))
                {
                    out.focusUnderDefView = true;
                    break;
                }
            }
        }
        if (gti.hwndCaret)
        {
            out.hasCaret = true;
            GetClassNameW(gti.hwndCaret, out.caretClass, static_cast<int>(std::size(out.caretClass)));
        }
    }
}
