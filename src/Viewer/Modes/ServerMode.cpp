#include "pch.h"
#include "Modes/Modes.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <thread>

#include "Diagnostics/Log.h"
#include "Ipc/IpcServer.h"
#include "Lifecycle/Lifecycle.h"
#include "System/AppPaths.h"
#include "Text/Utf.h"
#include "Util/Foreground.h"
#include "Util/Trim.h"
#include "ViewerHost.h"

namespace fastlook::viewer
{
    using core::ipc::Message;
    using core::ipc::MsgType;

    namespace
    {
        constexpr wchar_t kInstanceMutex[] = L"Local\\FastLook-Viewer";
        constexpr DWORD kExitWatchdogMs = 2000;

        std::optional<uint64_t> EnvNumber(wchar_t const* name)
        {
            wchar_t buffer[24]{};
            DWORD const n = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
            if (n == 0 || n >= std::size(buffer))
            {
                return std::nullopt;
            }
            wchar_t* end = nullptr;
            unsigned long long const value = wcstoull(buffer, &end, 10);
            if (end == buffer || *end != L'\0')
            {
                return std::nullopt;
            }
            return value;
        }

        struct Server : std::enable_shared_from_this<Server>
        {
            core::Lifecycle lifecycle;
            std::unique_ptr<IpcServer> ipc;
            winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer tick{nullptr};
            core::LanguagePref appliedLanguage;
            int pinned = 0;
            bool exitStarted = false;
            bool stallPending = false; // FASTLOOK_TEST_UI_STALL_MS: stall the UI thread once, on the first `show`

            Server(uint64_t idleMs, core::LanguagePref language) : lifecycle(idleMs, Now()), appliedLanguage(language) {}

            static uint64_t Now() { return GetTickCount64(); }

            uint64_t IdleMs() const
            {
                if (auto const seconds = EnvNumber(L"FASTLOOK_IDLE_SECONDS"))
                {
                    return *seconds * 1000;
                }
                return static_cast<uint64_t>(ViewerHost::Get().Settings().general.idleUnloadMinutes) * 60000;
            }

            void OnSettingsReloaded()
            {
                lifecycle.SetIdleMs(IdleMs());
                if (ViewerHost::Get().Settings().general.language != appliedLanguage)
                {
                    // The UI language is fixed at process start: restart as soon as nothing is open.
                    FL_LOG_INFO(L"UI language changed; exiting when quiet");
                    lifecycle.SetExitWhenQuiet();
                }
            }

            void Raise(core::ViewerEvent event)
            {
                Apply(lifecycle.On(event, Now()));
            }

            void Apply(core::LifecycleAction const& action)
            {
                if (action.trimNow)
                {
                    TrimWorkingSet(); // the GPU device stays (decision 55)
                }
                if (action.exitNow)
                {
                    Exit();
                }
            }

            // IPC first, so requests already queued on the UI thread are dropped without an ack and the daemon relaunches
            // (decision 53); the watchdog guarantees the process ends even if a window refuses to close.
            void Exit()
            {
                if (std::exchange(exitStarted, true))
                {
                    return;
                }
                FL_LOG_INFO(L"Server exit sequence");
                std::thread([] {
                    Sleep(kExitWatchdogMs);
                    TerminateProcess(GetCurrentProcess(), 0);
                }).detach();
                if (tick)
                {
                    tick.Stop();
                }
                if (ipc)
                {
                    ipc->Stop();
                }
                auto& host = ViewerHost::Get();
                host.CloseAllWindows();
                host.ExitApp();
            }

            void ForegroundPreview()
            {
                auto& host = ViewerHost::Get();
                ForceForeground(host.Window()->Hwnd());
            }

            void StallOnce()
            {
                if (std::exchange(stallPending, false))
                {
                    if (auto const ms = EnvNumber(L"FASTLOOK_TEST_UI_STALL_MS"))
                    {
                        Sleep(static_cast<DWORD>(*ms));
                    }
                }
            }

            std::optional<Message> Handle(Message const& request)
            {
                auto& host = ViewerHost::Get();
                Message reply;
                reply.type = MsgType::Ack;
                switch (request.type)
                {
                case MsgType::Show:
                    StallOnce();
                    {
                        // A hidden->visible show is brought to the front by PreviewSession; only a re-show of the visible
                        // preview needs it here.
                        bool const wasVisible = host.IsPreviewVisible();
                        host.Show(request.show);
                        if (wasVisible)
                        {
                            ForegroundPreview();
                        }
                    }
                    reply.shown = true;
                    return reply;
                case MsgType::Toggle:
                    reply.shown = host.Toggle(request.show); // a toggle only ever shows from hidden: PreviewSession owns the focus
                    return reply;
                case MsgType::Items:
                    host.UpdateItems(request.items);
                    reply.shown = host.IsPreviewVisible();
                    return reply;
                case MsgType::Ping:
                    reply.type = MsgType::Pong;
                    return reply;
                case MsgType::SettingsChanged:
                    host.ReloadSettings();
                    OnSettingsReloaded();
                    reply.shown = host.IsPreviewVisible();
                    return reply;
                case MsgType::OpenSettings:
                    host.OpenSettingsWindow(core::FromUtf8(request.page));
                    reply.shown = host.IsPreviewVisible();
                    return reply;
                case MsgType::Shutdown:
                    Exit();
                    return std::nullopt;
                default:
                    return std::nullopt;
                }
            }
        };

        std::wstring PipeName(core::ViewerArgs const& args)
        {
            return args.pipeName.empty() ? core::ipc::PipeNameForCurrentUser() : args.pipeName;
        }
    }

    void RunServerMode(core::ViewerArgs const& args)
    {
        auto& host = ViewerHost::Get();

        // The mutex lives as long as the process; the OS releases it on exit.
        HANDLE const instance = CreateMutexW(nullptr, FALSE, kInstanceMutex);
        if (instance == nullptr)
        {
            FL_LOG_INFO(L"Creating the instance mutex failed, error {}", GetLastError());
            ExitProcess(3);
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            FL_LOG_INFO(L"Another Viewer server is running");
            ExitProcess(2);
        }

        auto const ui = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        auto const paths = core::CurrentAppPaths();
        auto const settings = core::LoadSettings(paths.settingsFile).values;

        auto server = std::make_shared<Server>(0, settings.general.language);
        server->stallPending = EnvNumber(L"FASTLOOK_TEST_UI_STALL_MS").has_value();
        // The pipe exists before any XAML work, so the daemon can connect while the window frame is still being built.
        server->ipc = std::make_unique<IpcServer>(PipeName(args), ui, [server](Message const& request) { return server->Handle(request); });
        server->ipc->Start();

        host.Initialize(paths, CloseBehavior::HideWarm, args);
        server->lifecycle.SetIdleMs(server->IdleMs());

        // `server` is held by these callbacks for the life of the process (the host is a leaked singleton as well).
        HostCallbacks callbacks;
        callbacks.previewShown = [server] { server->Raise(core::ViewerEvent::PreviewShown); };
        callbacks.previewHidden = [server] { server->Raise(core::ViewerEvent::PreviewHidden); };
        callbacks.prewarmDone = [server] { server->Raise(core::ViewerEvent::PrewarmDone); };
        callbacks.settingsWindowOpen = [server](bool open) {
            server->Raise(open ? core::ViewerEvent::SettingsOpened : core::ViewerEvent::SettingsClosed);
        };
        callbacks.pinnedCountChanged = [server](int count) {
            // Lifecycle counts single opens/closes: replay the whole difference so it always equals `count`.
            int const previous = std::exchange(server->pinned, count);
            for (int i = previous; i < count; ++i)
            {
                server->Raise(core::ViewerEvent::PinnedOpened);
            }
            for (int i = previous; i > std::max(count, 0); --i)
            {
                server->Raise(core::ViewerEvent::PinnedClosed);
            }
            if (count > previous)
            {
                server->ipc->SendEvent("pinned");
            }
        };
        callbacks.settingsSaved = [server] {
            server->OnSettingsReloaded();
            server->ipc->SendEvent("settingsChanged");
        };
        host.SetCallbacks(std::move(callbacks));

        // The window frame is built up front; `--background` additionally renders a first frame off-screen without taking
        // focus (decision 26) and reports prewarmDone, otherwise the window counts as warm right away.
        host.Window();
        if (args.background)
        {
            host.PrewarmHidden();
        }
        else
        {
            server->Raise(core::ViewerEvent::PrewarmDone);
        }

        server->tick = ui.CreateTimer();
        server->tick.Interval(std::chrono::seconds(1));
        server->tick.IsRepeating(true);
        server->tick.Tick([server](auto const&, auto const&) { server->Raise(core::ViewerEvent::Tick); });
        server->tick.Start();
    }
}
