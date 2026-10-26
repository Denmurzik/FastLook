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

    // True once no process holds the daemon mutex (it disappears with the last handle, i.e. at process exit) before the deadline.
    bool WaitMutexGone(ULONGLONG deadline)
    {
        for (;;)
        {
            HANDLE const mutex = OpenMutexW(SYNCHRONIZE, FALSE, daemon::kMutexName);
            if (!mutex)
            {
                return true;
            }
            CloseHandle(mutex);
            if (GetTickCount64() >= deadline)
            {
                return false;
            }
            Sleep(50);
        }
    }

    // Exit 0 only when the daemon process is really gone (its mutex released), 1 if it is still alive after kShutdownWaitMs.
    // The daemon may hold its mutex before it has created its window, so retry only when an instance exists. A window that is
    // already gone while the mutex is still held means the process is still exiting: wait for the mutex, do not report "gone".
    int Shutdown()
    {
        HANDLE const mutex = OpenMutexW(SYNCHRONIZE, FALSE, daemon::kMutexName);
        bool const instanceExists = mutex != nullptr;
        if (mutex)
        {
            CloseHandle(mutex);
        }
        HWND const wnd = FindDaemonWindow(instanceExists ? kFindRetries : 0);
        ULONGLONG const deadline = GetTickCount64() + kShutdownWaitMs;
        if (!wnd)
        {
            return !instanceExists || WaitMutexGone(deadline) ? 0 : 1;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(wnd, &pid);
        HANDLE const process = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
        Forward(wnd, daemon::kCopyShutdown);
        if (process)
        {
            DWORD const waited = WaitForSingleObject(process, kShutdownWaitMs);
            CloseHandle(process);
            if (waited != WAIT_OBJECT_0)
            {
                return 1;
            }
        }
        else
        {
            // No handle to wait on: wait for the window to disappear instead.
            while (IsWindow(wnd))
            {
                if (GetTickCount64() >= deadline)
                {
                    return 1;
                }
                Sleep(50);
            }
        }
        // Stale pid or a second instance: the mutex is the authority.
        return WaitMutexGone(deadline) ? 0 : 1;
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
        if (args.showPath.empty())
        {
            code = 1; // malformed: no path
            break;
        }
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
