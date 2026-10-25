#include "Shell/ExplorerQuery.h"

#include <ShlObj.h>
#include <exdisp.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <utility>

#include "Nav/NavList.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")

namespace fastlook::core
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr UINT kBatch = 512;

        // Owns a CoTaskMem-allocated item ID list. The shell's PIDL types differ only by __unaligned and
        // strict-typing tags, so the typed views are explicit.
        class Pidl
        {
        public:
            Pidl() = default;
            explicit Pidl(void* raw) noexcept : m_raw(raw) {}
            Pidl(Pidl&& o) noexcept : m_raw(std::exchange(o.m_raw, nullptr)) {}
            Pidl& operator=(Pidl&& o) noexcept
            {
                if (this != &o)
                {
                    Release();
                    m_raw = std::exchange(o.m_raw, nullptr);
                }
                return *this;
            }
            ~Pidl() { Release(); }
            explicit operator bool() const noexcept { return m_raw != nullptr; }
            void Reset(void* raw) noexcept
            {
                Release();
                m_raw = raw;
            }
            PCUITEMID_CHILD Child() const noexcept { return reinterpret_cast<PCUITEMID_CHILD>(m_raw); }
            PCIDLIST_ABSOLUTE Absolute() const noexcept { return reinterpret_cast<PCIDLIST_ABSOLUTE>(m_raw); }

        private:
            void Release() noexcept
            {
                if (m_raw)
                    CoTaskMemFree(m_raw);
                m_raw = nullptr;
            }
            void* m_raw = nullptr;
        };

        bool IsDisconnected(HRESULT hr) noexcept
        {
            return hr == RPC_E_DISCONNECTED || hr == RPC_S_SERVER_UNAVAILABLE || hr == CO_E_OBJNOTCONNECTED;
        }

        // One IShellWindows per STA thread; dropped after Explorer restarts (the proxy is dead then).
        ComPtr<IShellWindows>& CachedShellWindows()
        {
            thread_local ComPtr<IShellWindows> cache;
            return cache;
        }

        ComPtr<IShellWindows> GetShellWindows()
        {
            auto& cache = CachedShellWindows();
            if (!cache)
                CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&cache));
            return cache;
        }

        void ResetShellWindows()
        {
            CachedShellWindows().Reset();
        }

        // The visible tab window of a CabinetWClass top-level window, or nullptr (not an Explorer window / no tabs).
        HWND ActiveTabWindow(HWND topLevel)
        {
            for (HWND child = nullptr; (child = FindWindowExW(topLevel, child, L"ShellTabWindowClass", nullptr)) != nullptr;)
            {
                if (IsWindowVisible(child))
                    return child;
            }
            return nullptr;
        }

        bool BrowserWindow(IShellBrowser* browser, HWND& hwnd)
        {
            ComPtr<IOleWindow> ow;
            return SUCCEEDED(browser->QueryInterface(IID_PPV_ARGS(&ow))) && SUCCEEDED(ow->GetWindow(&hwnd));
        }

        ComPtr<IShellBrowser> TopLevelBrowser(IUnknown* unk)
        {
            ComPtr<IServiceProvider> sp;
            ComPtr<IShellBrowser> browser;
            if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&sp))))
                sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser));
            return browser;
        }

        // Finds the IShellBrowser of the active tab of the Explorer window `topLevel`.
        ComPtr<IShellBrowser> FindWindowBrowser(IShellWindows* windows, HWND topLevel, HRESULT& hr)
        {
            long count = 0;
            hr = windows->get_Count(&count);
            if (FAILED(hr))
                return nullptr;
            HWND activeTab = ActiveTabWindow(topLevel);
            ComPtr<IShellBrowser> firstMatch;
            for (long i = 0; i < count; ++i)
            {
                VARIANT index{};
                index.vt = VT_I4;
                index.lVal = i;
                ComPtr<IDispatch> disp;
                if (FAILED(windows->Item(index, &disp)) || !disp)
                    continue;
                ComPtr<IWebBrowserApp> app;
                if (FAILED(disp.As(&app)))
                    continue;
                SHANDLE_PTR h = 0;
                if (FAILED(app->get_HWND(&h)) || reinterpret_cast<HWND>(h) != topLevel)
                    continue;
                auto browser = TopLevelBrowser(app.Get());
                if (!browser)
                    continue;
                if (!firstMatch)
                    firstMatch = browser;
                HWND bw = nullptr;
                if (activeTab && BrowserWindow(browser.Get(), bw) && bw == activeTab)
                    return browser;
            }
            return activeTab ? nullptr : firstMatch;
        }

        struct ViewContext
        {
            HWND explorer = nullptr;
            ComPtr<IShellView> view;
            ComPtr<IFolderView2> folderView;
            ComPtr<IShellFolder> folder; // bound locally from the folder PIDL: name lookups do not call into Explorer
            HWND viewWindow = nullptr;
        };

        bool OpenView(IShellBrowser* browser, HWND explorer, ViewContext& ctx)
        {
            ctx.explorer = explorer;
            if (FAILED(browser->QueryActiveShellView(&ctx.view)) || !ctx.view)
                return false;
            if (FAILED(ctx.view.As(&ctx.folderView)))
                return false;
            ctx.view->GetWindow(&ctx.viewWindow);
            ComPtr<IPersistFolder2> persist;
            if (FAILED(ctx.folderView->GetFolder(IID_PPV_ARGS(&persist))))
                return false;
            PIDLIST_ABSOLUTE raw = nullptr;
            if (FAILED(persist->GetCurFolder(&raw)) || !raw)
                return false;
            Pidl pidl(reinterpret_cast<void*>(raw));
            return SUCCEEDED(SHBindToObject(nullptr, pidl.Absolute(), nullptr, IID_PPV_ARGS(&ctx.folder)));
        }

        // One attempt: pattern is shared by the lookup of a foreground window and of a known Explorer HWND.
        bool TryOpen(HWND topLevel, bool isDesktop, ViewContext& ctx, HRESULT& hr)
        {
            auto windows = GetShellWindows();
            if (!windows)
            {
                hr = E_FAIL;
                return false;
            }
            ComPtr<IShellBrowser> browser;
            HWND explorer = topLevel;
            if (isDesktop)
            {
                VARIANT loc{};
                loc.vt = VT_I4;
                loc.lVal = CSIDL_DESKTOP;
                VARIANT root{};
                long hwnd = 0;
                ComPtr<IDispatch> disp;
                hr = windows->FindWindowSW(&loc, &root, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp);
                if (hr != S_OK || !disp)
                    return false;
                explorer = reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd));
                browser = TopLevelBrowser(disp.Get());
            }
            else
            {
                browser = FindWindowBrowser(windows.Get(), topLevel, hr);
            }
            if (!browser)
            {
                if (SUCCEEDED(hr))
                    hr = E_FAIL;
                return false;
            }
            if (!OpenView(browser.Get(), explorer, ctx))
            {
                hr = E_FAIL;
                return false;
            }
            hr = S_OK;
            return true;
        }

        bool OpenViewFor(HWND topLevel, bool isDesktop, ViewContext& ctx)
        {
            if (!isDesktop && topLevel)
                topLevel = GetAncestor(topLevel, GA_ROOT);
            HRESULT hr = S_OK;
            if (TryOpen(topLevel, isDesktop, ctx, hr))
                return true;
            if (IsDisconnected(hr))
            {
                ResetShellWindows(); // Explorer restarted
                ctx = {};
                return TryOpen(topLevel, isDesktop, ctx, hr);
            }
            return false;
        }

        std::optional<ipc::Item> ItemFromChild(ViewContext const& ctx, PCUITEMID_CHILD child)
        {
            STRRET sr{};
            if (FAILED(ctx.folder->GetDisplayNameOf(child, SHGDN_FORPARSING, &sr)))
                return std::nullopt;
            wchar_t name[32768];
            if (FAILED(StrRetToBufW(&sr, child, name, ARRAYSIZE(name))) || !name[0])
                return std::nullopt;
            SFGAOF attrs = SFGAO_FILESYSTEM;
            bool fileSystem = SUCCEEDED(ctx.folder->GetAttributesOf(1, &child, &attrs)) && (attrs & SFGAO_FILESYSTEM);
            ipc::Item item;
            item.path = name;
            item.isVirtual = !fileSystem;
            return item;
        }

        // Drains an item enumeration of the view in batches; `visit` returns false to stop early.
        template <class Visit>
        bool EnumerateView(ViewContext const& ctx, int flags, size_t cap, Visit&& visit)
        {
            ComPtr<IEnumIDList> en;
            if (FAILED(ctx.folderView->Items(static_cast<UINT>(flags), IID_PPV_ARGS(&en))) || !en)
                return false;
            size_t seen = 0;
            for (;;)
            {
                PITEMID_CHILD batch[kBatch] = {};
                ULONG fetched = 0;
                HRESULT hr = en->Next(kBatch, batch, &fetched);
                bool go = true;
                for (ULONG i = 0; i < fetched; ++i)
                {
                    Pidl owned(reinterpret_cast<void*>(batch[i]));
                    if (go && seen < cap)
                    {
                        ++seen;
                        go = visit(std::move(owned));
                    }
                }
                if (!go || seen >= cap || hr != S_OK || fetched == 0)
                    return true;
            }
        }

        Pidl CopyPidl(PCUIDLIST_RELATIVE src)
        {
            return Pidl(reinterpret_cast<void*>(ILClone(src)));
        }

        bool SamePidl(ViewContext const& ctx, PCUITEMID_CHILD a, PCUITEMID_CHILD b)
        {
            UINT sa = ILGetSize(a);
            if (sa == ILGetSize(b) && std::memcmp(a, b, sa) == 0)
                return true;
            HRESULT hr = ctx.folder->CompareIDs(SHCIDS_CANONICALONLY, a, b);
            return SUCCEEDED(hr) && static_cast<short>(HRESULT_CODE(hr)) == 0;
        }

        std::optional<RectI> IconRect(ViewContext const& ctx, PCUITEMID_CHILD child)
        {
            if (!ctx.viewWindow)
                return std::nullopt;
            POINT pt{};
            if (FAILED(ctx.folderView->GetItemPosition(child, &pt)))
                return std::nullopt;
            FOLDERVIEWMODE mode{};
            int iconSize = 0;
            if (FAILED(ctx.folderView->GetViewModeAndIconSize(&mode, &iconSize)) || iconSize <= 0)
                return std::nullopt;
            if (!ClientToScreen(ctx.viewWindow, &pt))
                return std::nullopt;
            RECT view{};
            if (!GetWindowRect(ctx.viewWindow, &view))
                return std::nullopt;
            RECT icon{pt.x, pt.y, pt.x + iconSize, pt.y + iconSize};
            RECT clipped{};
            if (!IntersectRect(&clipped, &view, &icon))
                return std::nullopt; // scrolled out of view
            return RectI{pt.x, pt.y, iconSize, iconSize};
        }
    }

    std::optional<ExplorerSnapshot> QueryExplorer(HWND foreground, bool isDesktop)
    {
        if (!foreground && !isDesktop)
        {
            GetShellWindows(); // prime the cache: the first cross-process activation is slow
            return std::nullopt;
        }
        ViewContext ctx;
        if (!OpenViewFor(foreground, isDesktop, ctx))
            return std::nullopt;

        ExplorerSnapshot snap;
        snap.explorer = ctx.explorer;
        snap.isDesktop = isDesktop;
        snap.monitor = MonitorFromWindow(ctx.explorer, MONITOR_DEFAULTTONEAREST);

        int total = 0;
        if (SUCCEEDED(ctx.folderView->ItemCount(SVGIO_ALLVIEW, &total)) && total > 0)
            snap.totalItems = static_cast<size_t>(total);

        Pidl focusPidl;
        int focusIndex = -1;
        if (ctx.folderView->GetFocusedItem(&focusIndex) == S_OK && focusIndex >= 0)
        {
            PITEMID_CHILD raw = nullptr;
            if (SUCCEEDED(ctx.folderView->Item(focusIndex, &raw)) && raw)
            {
                focusPidl.Reset(reinterpret_cast<void*>(raw));
                snap.focused = ItemFromChild(ctx, focusPidl.Child());
            }
        }

        Pidl firstSelected;
        EnumerateView(ctx, SVGIO_SELECTION | SVGIO_FLAG_VIEWORDER, nav::kMaxFolderItems, [&](Pidl pidl) {
            if (auto item = ItemFromChild(ctx, pidl.Child()))
            {
                snap.selection.push_back(std::move(*item));
                if (!firstSelected)
                    firstSelected = std::move(pidl);
            }
            return true;
        });

        PCUITEMID_CHILD iconTarget = focusPidl ? focusPidl.Child() : firstSelected.Child();
        if (iconTarget)
            snap.iconRect = IconRect(ctx, iconTarget);
        return snap;
    }

    std::optional<FolderList> EnumerateFolder(HWND explorer, bool isDesktop, ipc::Item const& anchor)
    {
        ViewContext ctx;
        if (!OpenViewFor(explorer, isDesktop, ctx))
            return std::nullopt;

        PIDLIST_ABSOLUTE rawAnchor = nullptr;
        if (FAILED(SHParseDisplayName(anchor.path.c_str(), nullptr, &rawAnchor, 0, nullptr)) || !rawAnchor)
            return std::nullopt;
        Pidl anchorAbs(reinterpret_cast<void*>(rawAnchor));
        Pidl anchorChild = CopyPidl(ILFindLastID(anchorAbs.Absolute()));
        if (!anchorChild)
            return std::nullopt;

        int total = 0;
        if (FAILED(ctx.folderView->ItemCount(SVGIO_ALLVIEW, &total)) || total <= 0)
            return std::nullopt;

        // Everything up to kMaxFolderItems; beyond that a ring of the last kWindowRadius PIDLs before the anchor
        // plus kWindowRadius after it.
        bool windowed = static_cast<size_t>(total) > nav::kMaxFolderItems;
        std::deque<Pidl> kept;
        size_t anchorPos = 0; // index of the anchor within `kept`
        bool found = false;
        size_t after = 0;
        EnumerateView(ctx, SVGIO_ALLVIEW | SVGIO_FLAG_VIEWORDER, SIZE_MAX, [&](Pidl pidl) {
            if (found)
            {
                kept.push_back(std::move(pidl));
                return ++after < nav::kWindowRadius || !windowed;
            }
            bool isAnchor = SamePidl(ctx, pidl.Child(), anchorChild.Child());
            kept.push_back(std::move(pidl));
            if (isAnchor)
            {
                found = true;
                anchorPos = kept.size() - 1;
            }
            else if (windowed && kept.size() > nav::kWindowRadius)
            {
                kept.pop_front();
            }
            return true;
        });
        if (!found)
            return std::nullopt;

        FolderList list;
        list.items.reserve(kept.size());
        for (size_t i = 0; i < kept.size(); ++i)
        {
            auto item = ItemFromChild(ctx, kept[i].Child());
            if (!item)
                continue;
            if (i == anchorPos)
                list.index = static_cast<uint32_t>(list.items.size());
            list.items.push_back(std::move(*item));
        }
        if (list.items.empty())
            return std::nullopt;
        return list;
    }

    bool SelectInExplorer(HWND explorer, bool isDesktop, ipc::Item const& item)
    {
        ViewContext ctx;
        if (!OpenViewFor(explorer, isDesktop, ctx))
            return false;
        PIDLIST_ABSOLUTE raw = nullptr;
        if (FAILED(SHParseDisplayName(item.path.c_str(), nullptr, &raw, 0, nullptr)) || !raw)
            return false;
        Pidl abs(reinterpret_cast<void*>(raw));
        PCUITEMID_CHILD child = ILFindLastID(abs.Absolute());
        if (!child)
            return false;
        return SUCCEEDED(ctx.view->SelectItem(child, SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE | SVSI_FOCUSED));
    }
}
