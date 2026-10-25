#include "PreviewController.h"

#include <algorithm>
#include <cwchar>
#include <format>
#include <iterator>

#include "DaemonApp.h"
#include "Diagnostics/Log.h"
#include "Diagnostics/PerfLog.h"
#include "Ipc/Protocol.h"
#include "Nav/NavList.h"
#include "Prewarm.h"
#include "Shell/ExplorerQuery.h"
#include "Shell/ShellWorker.h"
#include "Text/Utf.h"
#include "ViewerManager.h"

namespace fastlook::daemon
{
    using namespace std::chrono_literals;
    using core::ipc::Message;
    using core::ipc::MsgType;

    namespace
    {
        constexpr auto kShellTimeout = 1000ms;
        constexpr auto kPrimeTimeout = 3000ms; // the first IShellWindows use loads the shell; abandoning that thread would waste it
        constexpr auto kHeartbeatInterval = 1s;
        constexpr auto kTrimDelay = 5s;
        constexpr auto kShutdownWait = 4s;
        constexpr ULONGLONG kPrewarmMinIntervalMs = 5000;

        bool IsDesktopClass(HWND wnd)
        {
            wchar_t cls[64];
            return wnd && GetClassNameW(wnd, cls, static_cast<int>(std::size(cls))) > 0 &&
                   (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0);
        }

        uint64_t Handle64(void* handle)
        {
            return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
        }

        void LogAck(wchar_t const* what, std::optional<Message> const& ack)
        {
            if (!ack)
            {
                return;
            }
            int const shown = ack->shown ? 1 : 0;
            FL_LOG_INFO(L"{} shown={}", what, shown);
            core::perf::Mark(core::ToUtf8(what), std::format("shown={}", shown));
        }
    }

    PreviewController::PreviewController()
        : m_shell(std::make_unique<core::ShellWorker>()),
          m_viewer(std::make_unique<ViewerManager>([this](Message const& event) {
              if (event.eventName == "settingsChanged")
              {
                  if (m_main)
                  {
                      PostMessageW(m_main, WM_FL_SETTINGS_CHANGED, 0, 0);
                  }
              }
              else if (event.eventName == "pinned")
              {
                  FL_LOG_INFO(L"viewer event pinned");
              }
          })),
          m_prewarm(std::make_unique<Prewarm>([this](bool isDesktop) { OnForegroundCandidate(isDesktop); }))
    {
    }

    PreviewController::~PreviewController()
    {
        Shutdown();
    }

    void PreviewController::Start(HWND mainWnd, core::Settings const& settings, bool warm)
    {
        FL_LOG_INFO(L"previewController start warm={}", warm);
        m_main = mainWnd;
        {
            std::lock_guard lock(m_mutex);
            m_settings = settings;
            m_started = true;
            m_lastJobEnd = std::chrono::steady_clock::now();
            m_trimmed = false;
        }
        m_thread = std::thread([this] { ThreadMain(); });

        if (warm)
        {
            Post([this] { m_viewer->EnsureRunning(); });
        }
        // Spawns the STA thread and fills its IShellWindows cache; the result is irrelevant.
        Post([this] { m_shell->Run([] { return core::QueryExplorer(nullptr, false); }, kPrimeTimeout); });
        m_prewarm->Start(mainWnd);
    }

    void PreviewController::Stop()
    {
        Shutdown();
    }

    void PreviewController::Shutdown()
    {
        {
            std::lock_guard lock(m_mutex);
            if (!m_started || m_stop)
            {
                return;
            }
            m_stop = true;
            m_tasks.clear(); // only the viewer teardown still runs
            m_tasks.push_back([this] { m_viewer->Shutdown(); });
        }
        FL_LOG_INFO(L"previewController shutdown");
        m_cv.notify_all();
        m_prewarm->Stop();

        std::unique_lock lock(m_mutex);
        if (m_cv.wait_for(lock, kShutdownWait, [this] { return m_done; }))
        {
            lock.unlock();
            m_thread.join();
        }
        else
        {
            // A stuck pipe or shell call: members cannot be destroyed under the running thread. The process ends here;
            // the Job object takes the Viewer down.
            lock.unlock();
            FL_LOG_INFO(L"controller thread did not stop in time, exiting");
            ExitProcess(0);
        }
    }

    void PreviewController::Post(Task task)
    {
        {
            std::lock_guard lock(m_mutex);
            if (!m_started || m_stop)
            {
                return;
            }
            m_tasks.push_back(std::move(task));
        }
        m_cv.notify_all();
    }

    void PreviewController::ThreadMain()
    {
        using Clock = std::chrono::steady_clock;
        auto nextBeat = Clock::now() + kHeartbeatInterval;

        std::unique_lock lock(m_mutex);
        for (;;)
        {
            if (m_stop && m_tasks.empty())
            {
                break;
            }
            auto const now = Clock::now();
            if (now >= nextBeat)
            {
                nextBeat = now + kHeartbeatInterval;
                lock.unlock();
                m_viewer->Heartbeat();
                lock.lock();
                continue;
            }
            if (!m_trimmed && now >= m_lastJobEnd + kTrimDelay && m_tasks.empty())
            {
                m_trimmed = true;
                lock.unlock();
                // Shell namespace extensions loaded by the STA queries would otherwise stay resident.
                SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
                lock.lock();
                continue;
            }
            if (!m_tasks.empty())
            {
                Task task = std::move(m_tasks.front());
                m_tasks.pop_front();
                lock.unlock();
                try
                {
                    task();
                }
                catch (std::exception const&)
                {
                    FL_LOG_INFO(L"controller job threw");
                }
                lock.lock();
                m_lastJobEnd = Clock::now();
                m_trimmed = false;
                continue;
            }
            auto wake = nextBeat;
            if (!m_trimmed)
            {
                wake = std::min(wake, m_lastJobEnd + kTrimDelay);
            }
            m_cv.wait_until(lock, wake, [this] { return !m_tasks.empty() || m_stop; });
        }
        m_done = true;
        m_cv.notify_all();
    }

    void PreviewController::OnTrigger(HWND foreground)
    {
        core::perf::Mark("trigger_received");
        uint64_t const sequence = ++m_triggerSeq;
        Post([this, foreground, sequence] { RunTrigger(foreground, sequence); });
    }

    void PreviewController::RunTrigger(HWND foreground, uint64_t sequence)
    {
        bool const isDesktop = IsDesktopClass(foreground);
        auto snapshot = m_shell->Run([foreground, isDesktop] { return core::QueryExplorer(foreground, isDesktop); }, kShellTimeout);
        if (!snapshot || !*snapshot)
        {
            FL_LOG_INFO(L"trigger: Explorer query failed");
            return;
        }
        core::ExplorerSnapshot const& snap = **snapshot;
        auto plan = core::nav::PlanFromSelection(snap.selection, snap.focused);
        if (!plan)
        {
            FL_LOG_INFO(L"trigger: nothing selected");
            return;
        }

        Message toggle;
        toggle.type = MsgType::Toggle;
        toggle.show.explorerHwnd = Handle64(snap.explorer);
        toggle.show.isDesktop = snap.isDesktop;
        toggle.show.monitor = Handle64(snap.monitor);
        toggle.show.iconRect = snap.iconRect;
        toggle.show.items = plan->items;
        toggle.show.index = plan->index;
        toggle.show.navMode = plan->mode;
        toggle.show.listComplete = plan->listComplete;
        auto ack = m_viewer->Deliver(std::move(toggle));
        LogAck(L"toggle_acked", ack);
        if (!ack || !ack->shown || plan->listComplete || plan->index >= plan->items.size() || !m_viewer->IsRunning())
        {
            return;
        }

        // Phase 2: the full folder list. A newer trigger supersedes it.
        core::ipc::Item anchor = plan->items[plan->index];
        HWND const explorer = snap.explorer;
        bool const desktop = snap.isDesktop;
        auto folder = m_shell->Run([explorer, desktop, anchor] { return core::EnumerateFolder(explorer, desktop, anchor); }, kShellTimeout);
        if (!folder || !*folder)
        {
            return;
        }
        if (m_triggerSeq.load() != sequence)
        {
            FL_LOG_INFO(L"folder list dropped: newer trigger");
            return;
        }
        Message items;
        items.type = MsgType::Items;
        items.items.items = std::move((*folder)->items);
        items.items.index = (*folder)->index;
        items.items.navMode = core::ipc::NavMode::Folder;
        m_viewer->Deliver(std::move(items));
    }

    void PreviewController::SendSinglePath(std::wstring const& path, bool toggle)
    {
        Message msg;
        msg.type = toggle ? MsgType::Toggle : MsgType::Show;
        msg.show.explorerHwnd = 0;
        msg.show.items.push_back(core::ipc::Item{path, false});
        msg.show.index = 0;
        msg.show.navMode = core::ipc::NavMode::Selection;
        msg.show.listComplete = true;
        auto ack = m_viewer->Deliver(std::move(msg));
        LogAck(toggle ? L"toggle_acked" : L"show_acked", ack);
    }

    void PreviewController::ShowPath(std::wstring const& path)
    {
        FL_LOG_INFO(L"showPath {}", core::log::RedactPath(path));
        Post([this, path] { SendSinglePath(path, false); });
    }

    void PreviewController::TogglePath(std::wstring const& path)
    {
        FL_LOG_INFO(L"togglePath {}", core::log::RedactPath(path));
        Post([this, path] { SendSinglePath(path, true); });
    }

    void PreviewController::OpenSettings(std::wstring_view page)
    {
        FL_LOG_INFO(L"openSettings page={}", page);
        Post([this, page = core::ToUtf8(page)] {
            Message msg;
            msg.type = MsgType::OpenSettings;
            msg.page = page;
            m_viewer->Deliver(std::move(msg));
        });
    }

    void PreviewController::OnSettingsChanged(core::Settings const& settings)
    {
        {
            std::lock_guard lock(m_mutex);
            m_settings = settings;
        }
        Post([this] {
            if (m_viewer->IsRunning())
            {
                Message msg;
                msg.type = MsgType::SettingsChanged;
                m_viewer->Deliver(std::move(msg));
            }
        });
    }

    bool PreviewController::PrewarmAllowed(bool isDesktop)
    {
        core::Settings settings;
        {
            std::lock_guard lock(m_mutex);
            settings = m_settings;
        }
        return settings.general.prewarm && (!isDesktop || settings.general.desktopEnabled);
    }

    void PreviewController::OnForegroundCandidate(bool isDesktop)
    {
        // Main thread: cheap checks only; the controller thread decides whether the Viewer needs starting.
        if (!PrewarmAllowed(isDesktop))
        {
            return;
        }
        ULONGLONG const now = GetTickCount64();
        if (m_lastPrewarmTick != 0 && now - m_lastPrewarmTick < kPrewarmMinIntervalMs)
        {
            return;
        }
        m_lastPrewarmTick = now;
        Post([this] {
            if (!m_viewer->IsRunning())
            {
                FL_LOG_INFO(L"prewarm");
                m_viewer->EnsureRunning();
            }
        });
    }
}
