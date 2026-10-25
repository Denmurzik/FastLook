#include <windows.h>
#include <shellapi.h>

#include <string>
#include <string_view>
#include <vector>

#include "Cli/DaemonArgs.h"
#include "DaemonApp.h"

using namespace fastlook;

namespace
{
    constexpr DWORD kForwardTimeoutMs = 3000;
    constexpr DWORD kShutdownWaitMs = 5000;
    // A second instance may be launched while the first one is still creating its window.
    constexpr int kFindRetries = 20;
    constexpr DWORD kFindRetryDelayMs = 100;

    HWND FindDaemonWindow(int retries)
    {
        for (int i = 0;; ++i)
        {
            if (HWND wnd = FindWindowW(daemon::kWindowClass, nullptr))
            {
                return wnd;
            }
            if (i >= retries)
            {
                return nullptr;
            }
            Sleep(kFindRetryDelayMs);
        }
    }

    bool Forward(HWND wnd, ULONG_PTR kind, std::wstring const& path = {})
    {
        COPYDATASTRUCT cds{};
        cds.dwData = kind;
        cds.cbData = static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t));
        cds.lpData = path.empty() ? nullptr : const_cast<wchar_t*>(path.c_str());
        if (path.empty())
        {
            cds.cbData = 0;
        }
        DWORD_PTR result = 0;
        return SendMessageTimeoutW(wnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, kForwardTimeoutMs, &result) != 0 &&
               result != 0;
    }

    int Shutdown()
    {
        HWND const wnd = FindDaemonWindow(0);
        if (!wnd)
        {
            return 0;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(wnd, &pid);
        HANDLE const process = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
        Forward(wnd, daemon::kCopyShutdown);
        int code = 0;
        if (process)
        {
            code = WaitForSingleObject(process, kShutdownWaitMs) == WAIT_OBJECT_0 ? 0 : 1;
            CloseHandle(process);
        }
        return code;
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring_view> views;
    for (int i = 1; argv && i < argc; ++i)
    {
        views.emplace_back(argv[i]);
    }
    core::DaemonArgs const args = core::ParseDaemonArgs(views);

    int code = 0;
    switch (args.command)
    {
    case core::DaemonCommand::Shutdown:
        code = Shutdown();
        break;
    case core::DaemonCommand::Show:
    case core::DaemonCommand::Toggle:
    {
        HWND const wnd = FindDaemonWindow(kFindRetries);
        bool const show = args.command == core::DaemonCommand::Show;
        code = wnd && Forward(wnd, show ? daemon::kCopyShow : daemon::kCopyToggle, args.showPath) ? 0 : 1;
        break;
    }
    case core::DaemonCommand::Run:
    {
        HANDLE const mutex = CreateMutexW(nullptr, FALSE, daemon::kMutexName);
        if (!mutex)
        {
            code = 1;
        }
        else if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            HWND const wnd = FindDaemonWindow(kFindRetries);
            code = wnd && Forward(wnd, daemon::kCopyOpenSettings) ? 0 : 1;
        }
        else
        {
            daemon::DaemonApp app;
            code = app.Run(args);
        }
        break;
    }
    }

    if (argv)
    {
        LocalFree(argv);
    }
    return code;
}
