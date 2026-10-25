#include "FocusProbe.h"

#include <cwchar>
#include <iterator>

#include "../SharedState.h"

namespace fastlook::daemon
{
    namespace
    {
        constexpr int kMaxAncestorLevels = 8;
        constexpr size_t kCacheSize = 4;

        // Hook thread only (thread_local: a relaunched hook thread starts clean). Fixed size, no allocation, no locks.
        // Entries are keyed by HWND plus the owning thread (and the Viewer pid for the foreground), so a recycled handle
        // does not match a stale entry; the focus entry also compares the class name, which is read on every probe.
        struct ForegroundEntry
        {
            HWND hwnd = nullptr;
            DWORD thread = 0;
            DWORD viewerPid = 0;
            core::hook::ForegroundKind kind = core::hook::ForegroundKind::Other;
        };

        struct FocusEntry
        {
            HWND hwnd = nullptr;
            DWORD thread = 0;
            wchar_t cls[64]{};
            bool underDefView = false;
        };

        thread_local ForegroundEntry t_fgCache[kCacheSize];
        thread_local size_t t_fgNext = 0;
        thread_local FocusEntry t_focusCache[kCacheSize];
        thread_local size_t t_focusNext = 0;

        bool ClassIs(HWND wnd, wchar_t const* name) noexcept
        {
            wchar_t buf[64];
            return GetClassNameW(wnd, buf, static_cast<int>(std::size(buf))) > 0 && wcscmp(buf, name) == 0;
        }

        core::hook::ForegroundKind ClassifyUncached(HWND fg, DWORD pid, DWORD viewerPid) noexcept
        {
            using core::hook::ForegroundKind;
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
            return viewerPid != 0 && pid == viewerPid ? ForegroundKind::OurPreview : ForegroundKind::Other;
        }

        core::hook::ForegroundKind ClassifyCached(HWND fg, DWORD thread, DWORD pid) noexcept
        {
            DWORD const viewerPid = g_viewerPid.load(std::memory_order_relaxed);
            for (auto const& e : t_fgCache)
            {
                // A WorkerW only gains its SHELLDLL_DefView child some time after it is created: never cache it as Other.
                if (e.hwnd == fg && e.thread == thread && e.viewerPid == viewerPid)
                {
                    return e.kind;
                }
            }
            auto const kind = ClassifyUncached(fg, pid, viewerPid);
            if (kind != core::hook::ForegroundKind::Other) // Other may be a desktop window still being built
            {
                t_fgCache[t_fgNext] = {fg, thread, viewerPid, kind};
                t_fgNext = (t_fgNext + 1) % kCacheSize;
            }
            return kind;
        }

        // GetClassName of the focus window is always read; only the ancestor walk (up to 8 GetClassName calls) is cached.
        bool UnderDefView(HWND focus, DWORD thread, wchar_t const* cls) noexcept
        {
            for (auto const& e : t_focusCache)
            {
                if (e.hwnd == focus && e.thread == thread && wcscmp(e.cls, cls) == 0)
                {
                    return e.underDefView;
                }
            }
            bool found = false;
            HWND wnd = focus;
            for (int i = 0; wnd && i < kMaxAncestorLevels; ++i, wnd = GetParent(wnd))
            {
                if (ClassIs(wnd, L"SHELLDLL_DefView"))
                {
                    found = true;
                    break;
                }
            }
            FocusEntry& slot = t_focusCache[t_focusNext];
            t_focusNext = (t_focusNext + 1) % kCacheSize;
            slot.hwnd = focus;
            slot.thread = thread;
            wcsncpy_s(slot.cls, cls, _TRUNCATE);
            slot.underDefView = found;
            return found;
        }
    }

    core::hook::ForegroundKind ClassifyForeground(HWND fg) noexcept
    {
        if (!fg)
        {
            return core::hook::ForegroundKind::Other;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        return ClassifyUncached(fg, pid, g_viewerPid.load(std::memory_order_relaxed));
    }

    void ProbeFocus(HWND fg, core::hook::FocusSnapshot& out) noexcept
    {
        out = {};
        if (!fg)
        {
            return;
        }
        DWORD pid = 0;
        DWORD const thread = GetWindowThreadProcessId(fg, &pid);
        out.fg = ClassifyCached(fg, thread, pid);
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
            out.focusUnderDefView = UnderDefView(gti.hwndFocus, thread, out.focusClass);
        }
        if (gti.hwndCaret)
        {
            out.hasCaret = true;
            GetClassNameW(gti.hwndCaret, out.caretClass, static_cast<int>(std::size(out.caretClass)));
        }
    }
}
