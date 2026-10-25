#include "DaemonApp.h"

#include <wtsapi32.h>

#include <filesystem>
#include <format>

#include "Diagnostics/Log.h"
#include "Diagnostics/PerfLog.h"
#include "PeekNotice.h"
#include "SharedState.h"
#include "Strings.h"
#include "System/AppPaths.h"

#pragma comment(lib, "wtsapi32.lib")

namespace fastlook::daemon
{
    namespace
    {
        constexpr UINT_PTR kWatchdogTimer = 1;
        constexpr UINT kWatchdogIntervalMs = 1000;
        // Explorer may not have created the notification area yet (autostart at logon).
        constexpr UINT_PTR kTrayRetryTimer = 2;
        constexpr UINT kTrayRetryIntervalMs = 2000;
        constexpr int kTrayRetryMax = 30;
        constexpr DWORD kMaxCopyPathBytes = 64 * 1024;

        // Payload is a UTF-16 path, possibly NUL-terminated.
        bool ReadPath(COPYDATASTRUCT const& cds, std::wstring& out)
        {
            if (!cds.lpData || cds.cbData == 0 || cds.cbData % sizeof(wchar_t) != 0 || cds.cbData > kMaxCopyPathBytes)
            {
                return false;
            }
            auto const* chars = static_cast<wchar_t const*>(cds.lpData);
            size_t len = cds.cbData / sizeof(wchar_t);
            while (len > 0 && chars[len - 1] == L'\0')
            {
                --len;
            }
            out.assign(chars, len);
            return !out.empty();
        }
    }

    int DaemonApp::Run(core::DaemonArgs const& args)
    {
        auto const paths = core::CurrentAppPaths();
        std::error_code ec;
        std::filesystem::create_directories(paths.logsDir, ec);

        auto doc = core::LoadSettings(paths.settingsFile);
        m_settings = doc.values;
        core::log::Init(paths.logsDir, L"daemon", m_settings.logLevel);
        if (!args.perfLog.empty())
        {
            core::perf::Open(args.perfLog);
        }
        FL_LOG_INFO(L"daemon start");
        SetLanguage(m_settings.general.language);
        if (doc.readFailed)
        {
            FL_LOG_INFO(L"settings file could not be read, using defaults without saving");
        }
        else if (doc.recoveredFromCorrupt)
        {
            FL_LOG_INFO(L"settings file was corrupt, backed up and reset to defaults");
        }

        HINSTANCE const inst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &DaemonApp::WndProc;
        wc.hInstance = inst;
        wc.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&wc))
        {
            FL_LOG_INFO(L"RegisterClassEx failed {}", GetLastError());
            return 1;
        }
        m_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

        m_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"FastLook", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, this);
        if (!m_wnd)
        {
            FL_LOG_INFO(L"CreateWindowEx failed {}", GetLastError());
            return 1;
        }

        m_peekPending = DetectPeekConflict(m_settings);
        AddTray();
        WTSRegisterSessionNotification(m_wnd, NOTIFY_FOR_THIS_SESSION);
        HPOWERNOTIFY const powerNotify = RegisterSuspendResumeNotification(m_wnd, DEVICE_NOTIFY_WINDOW_HANDLE);
        SetTimer(m_wnd, kWatchdogTimer, kWatchdogIntervalMs, nullptr);

        m_hook.Start(m_wnd, MakeHookConfig());
        m_controller.Start(m_wnd, m_settings, args.warm);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (powerNotify)
        {
            UnregisterSuspendResumeNotification(powerNotify);
        }
        WTSUnRegisterSessionNotification(m_wnd);
        DestroyWindow(m_wnd);
        FL_LOG_INFO(L"daemon exit");
        return 0;
    }

    void DaemonApp::AddTray()
    {
        if (m_tray.Add(m_wnd, WM_FL_TRAY))
        {
            if (m_trayRetrying)
            {
                KillTimer(m_wnd, kTrayRetryTimer);
                m_trayRetrying = false;
            }
            if (m_peekPending)
            {
                m_peekPending = false;
                ShowPeekNotice(m_tray);
            }
        }
        else if (!m_trayRetrying)
        {
            m_trayRetrying = true;
            m_trayRetries = 0;
            SetTimer(m_wnd, kTrayRetryTimer, kTrayRetryIntervalMs, nullptr);
        }
    }

    core::hook::HookConfig DaemonApp::MakeHookConfig() const
    {
        core::hook::HookConfig c;
        c.key = m_settings.general.activationKey;
        c.desktopEnabled = m_settings.general.desktopEnabled;
        c.paused = m_paused;
        return c;
    }

    void DaemonApp::ReloadSettings()
    {
        auto doc = core::LoadSettings(core::CurrentAppPaths().settingsFile);
        if (doc.recoveredFromCorrupt)
        {
            FL_LOG_INFO(L"settings file was corrupt, backed up and reset to defaults");
        }
        if (doc.readFailed)
        {
            FL_LOG_INFO(L"settings reload failed, keeping previous values");
            return;
        }
        m_settings = doc.values;
        core::log::SetLevel(m_settings.logLevel);
        SetLanguage(m_settings.general.language);
        m_tray.SetPaused(m_paused); // refresh the tooltip in the new language
        m_hook.SetConfig(MakeHookConfig());
        m_controller.OnSettingsChanged(m_settings);
    }

    void DaemonApp::Exit()
    {
        if (m_exiting)
        {
            return;
        }
        m_exiting = true;
        KillTimer(m_wnd, kWatchdogTimer);
        KillTimer(m_wnd, kTrayRetryTimer);
        m_controller.Shutdown();
        m_hook.Stop();
        m_tray.Remove();
        PostQuitMessage(0);
    }

    void DaemonApp::OnCommand(UINT id)
    {
        switch (id)
        {
        case kCmdSettings:
            m_controller.OpenSettings();
            break;
        case kCmdPause:
            m_paused = !m_paused;
            m_hook.SetConfig(MakeHookConfig());
            m_tray.SetPaused(m_paused);
            break;
        case kCmdExit:
            Exit();
            break;
        default:
            break;
        }
    }

    bool DaemonApp::OnCopyData(COPYDATASTRUCT const& cds)
    {
        std::wstring path;
        switch (cds.dwData)
        {
        case kCopyOpenSettings:
            m_controller.OpenSettings();
            return true;
        case kCopyShow:
            if (ReadPath(cds, path))
            {
                m_controller.ShowPath(path);
                return true;
            }
            return false;
        case kCopyToggle:
            if (ReadPath(cds, path))
            {
                m_controller.TogglePath(path);
                return true;
            }
            return false;
        case kCopyShutdown:
            // Posted, so the sender's SendMessage returns before the teardown starts.
            PostMessageW(m_wnd, WM_COMMAND, kCmdExit, 0);
            return true;
        default:
            return false;
        }
    }

    LRESULT CALLBACK DaemonApp::WndProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCCREATE)
        {
            auto const* cs = reinterpret_cast<CREATESTRUCTW const*>(lParam);
            SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<DaemonApp*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
        if (!self)
        {
            return DefWindowProcW(wnd, msg, wParam, lParam);
        }
        return self->OnMessage(wnd, msg, wParam, lParam);
    }

    LRESULT DaemonApp::OnMessage(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (m_taskbarCreated != 0 && msg == m_taskbarCreated)
        {
            AddTray();
            m_hook.Rehook("taskbar");
            return 0;
        }

        switch (msg)
        {
        case WM_FL_TRIGGER:
            core::perf::Mark("hook_trigger", std::format("qpc_at_hook={}", g_lastTriggerQpc.load()));
            m_controller.OnTrigger(reinterpret_cast<HWND>(wParam));
            return 0;
        case WM_FL_TRAY:
            switch (LOWORD(lParam))
            {
            case WM_LBUTTONDBLCLK:
                m_controller.OpenSettings();
                break;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                m_tray.ShowMenu(wnd);
                break;
            default:
                break;
            }
            return 0;
        case WM_FL_SETTINGS_CHANGED:
            ReloadSettings();
            return 0;
        case WM_COMMAND:
            OnCommand(LOWORD(wParam));
            return 0;
        case WM_COPYDATA:
            return OnCopyData(*reinterpret_cast<COPYDATASTRUCT const*>(lParam)) ? TRUE : FALSE;
        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMEAUTOMATIC)
            {
                m_hook.Rehook("resume");
            }
            return TRUE;
        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_UNLOCK)
            {
                m_hook.Rehook("unlock");
            }
            else if (wParam == WTS_CONSOLE_CONNECT || wParam == WTS_REMOTE_CONNECT)
            {
                m_hook.Rehook("reconnect");
            }
            return 0;
        case WM_TIMER:
            if (wParam == kWatchdogTimer)
            {
                m_hook.CheckWatchdog();
            }
            else if (wParam == kTrayRetryTimer && m_trayRetrying)
            {
                if (++m_trayRetries > kTrayRetryMax)
                {
                    KillTimer(wnd, kTrayRetryTimer);
                    m_trayRetrying = false;
                    FL_LOG_INFO(L"tray add gave up");
                }
                else
                {
                    AddTray();
                }
            }
            return 0;
        case WM_ENDSESSION:
            if (wParam)
            {
                Exit();
            }
            return 0;
        default:
            return DefWindowProcW(wnd, msg, wParam, lParam);
        }
    }
}
