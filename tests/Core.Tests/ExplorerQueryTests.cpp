// End-to-end: opens one real Explorer window on a temp folder and closes it again. Runs only with FASTLOOK_E2E=1.
#include <Windows.h>
#include <ShlObj.h>
#include <exdisp.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <chrono>
#include <filesystem>
#include <string>

#include "Shell/ExplorerQuery.h"
#include "Shell/ShellWorker.h"
#include "TestUtil.h"

using namespace fastlook::core;
using namespace std::chrono_literals;
using Microsoft::WRL::ComPtr;

namespace
{
    struct Located
    {
        HWND hwnd = nullptr;
        ComPtr<IWebBrowser2> browser;
    };

    // STA only: the Explorer window whose address is `dir`.
    Located FindExplorerWindow(std::wstring const& dir)
    {
        ComPtr<IShellWindows> windows;
        if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows))))
            return {};
        long count = 0;
        windows->get_Count(&count);
        for (long i = 0; i < count; ++i)
        {
            VARIANT idx{};
            idx.vt = VT_I4;
            idx.lVal = i;
            ComPtr<IDispatch> disp;
            if (FAILED(windows->Item(idx, &disp)) || !disp)
                continue;
            ComPtr<IWebBrowser2> wb;
            if (FAILED(disp.As(&wb)))
                continue;
            BSTR url = nullptr;
            if (FAILED(wb->get_LocationURL(&url)) || !url)
                continue;
            wchar_t path[MAX_PATH * 2];
            DWORD len = ARRAYSIZE(path);
            bool match = SUCCEEDED(PathCreateFromUrlW(url, path, &len, 0)) && _wcsicmp(path, dir.c_str()) == 0;
            SysFreeString(url);
            if (!match)
                continue;
            SHANDLE_PTR h = 0;
            wb->get_HWND(&h);
            return {reinterpret_cast<HWND>(h), wb};
        }
        return {};
    }

    // STA only: selects (without focus change) the given file names in the view.
    bool SelectNames(IWebBrowser2* wb, std::wstring const& dir, std::vector<std::wstring> const& names)
    {
        ComPtr<IServiceProvider> sp;
        ComPtr<IShellBrowser> sb;
        ComPtr<IShellView> view;
        if (FAILED(wb->QueryInterface(IID_PPV_ARGS(&sp))) || FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb))) ||
            FAILED(sb->QueryActiveShellView(&view)))
            return false;
        bool first = true;
        for (auto const& n : names)
        {
            PIDLIST_ABSOLUTE abs = nullptr;
            if (FAILED(SHParseDisplayName((dir + L"\\" + n).c_str(), nullptr, &abs, 0, nullptr)))
                return false;
            HRESULT hr = view->SelectItem(ILFindLastID(abs), first ? (SVSI_SELECT | SVSI_DESELECTOTHERS) : SVSI_SELECT);
            CoTaskMemFree(abs);
            if (FAILED(hr))
                return false;
            first = false;
        }
        return true;
    }

    std::wstring Name(ipc::Item const& i)
    {
        return std::filesystem::path(i.path).filename().wstring();
    }
}

TEST(ExplorerQuery, SelectionFocusAndEnumeration)
{
    if (!GetEnvironmentVariableW(L"FASTLOOK_E2E", nullptr, 0))
        GTEST_SKIP();

    auto dirPath = MakeTempDir();
    for (auto const* n : {"a.txt", "b.txt", "c.txt"})
        std::ofstream(dirPath / n) << "x";
    std::wstring dir = std::filesystem::canonical(dirPath).wstring();

    ShellWorker worker;
    ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"\"" + dir + L"\"").c_str(), nullptr, SW_SHOWNORMAL);

    HWND hwnd = nullptr;
    for (int i = 0; i < 50 && !hwnd; ++i)
    {
        auto found = worker.Run([&] { return FindExplorerWindow(dir).hwnd; }, 3000ms);
        if (found)
            hwnd = *found;
        if (!hwnd)
            Sleep(200);
    }
    ASSERT_NE(hwnd, nullptr) << "Explorer window did not appear";

    auto quit = [&] {
        worker.Run(
            [&] {
                if (auto w = FindExplorerWindow(dir); w.browser)
                    w.browser->Quit();
                return 0;
            },
            3000ms);
    };

    // Wait for the view to populate, then select b and c.
    bool selected = false;
    for (int i = 0; i < 50 && !selected; ++i)
    {
        auto r = worker.Run(
            [&] {
                auto w = FindExplorerWindow(dir);
                if (!w.browser)
                    return false;
                auto snap = QueryExplorer(hwnd, false);
                if (!snap || snap->totalItems != 3)
                    return false;
                return SelectNames(w.browser.Get(), dir, {L"b.txt", L"c.txt"});
            },
            3000ms);
        selected = r && *r;
        if (!selected)
            Sleep(200);
    }
    if (!selected)
    {
        quit();
        FAIL() << "could not select items";
    }

    auto snap = worker.Run([&] { return QueryExplorer(hwnd, false); }, 3000ms);
    ASSERT_TRUE(snap && *snap);
    auto const& s = **snap;
    EXPECT_EQ(s.explorer, hwnd);
    EXPECT_FALSE(s.isDesktop);
    ASSERT_EQ(s.selection.size(), 2u);
    EXPECT_EQ(Name(s.selection[0]), L"b.txt");
    EXPECT_EQ(Name(s.selection[1]), L"c.txt");
    EXPECT_FALSE(s.selection[0].isVirtual);
    EXPECT_EQ(s.totalItems, 3u);
    EXPECT_NE(s.monitor, nullptr);

    auto list = worker.Run([&] { return EnumerateFolder(hwnd, false, ipc::Item{(std::filesystem::path(dir) / L"b.txt").wstring()}); }, 3000ms);
    ASSERT_TRUE(list && *list);
    ASSERT_EQ((*list)->items.size(), 3u);
    EXPECT_EQ((*list)->index, 1u);
    EXPECT_EQ(Name((*list)->items[0]), L"a.txt");

    auto sel = worker.Run([&] { return SelectInExplorer(hwnd, false, ipc::Item{(std::filesystem::path(dir) / L"a.txt").wstring()}); }, 3000ms);
    EXPECT_TRUE(sel && *sel);
    auto after = worker.Run([&] { return QueryExplorer(hwnd, false); }, 3000ms);
    quit();
    ASSERT_TRUE(after && *after);
    ASSERT_EQ((*after)->selection.size(), 1u);
    EXPECT_EQ(Name((*after)->selection[0]), L"a.txt");
}
