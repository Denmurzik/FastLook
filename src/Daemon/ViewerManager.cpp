#include "ViewerManager.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <string>

#include "Diagnostics/Log.h"
#include "SharedState.h"
#include "System/AppPaths.h"
#include "Text/Utf.h"

namespace fastlook::daemon
{
    using namespace std::chrono_literals;
    using core::ipc::Message;
    using core::ipc::MsgType;

    namespace
    {
        constexpr auto kConnectTimeout = 3000ms;
        constexpr auto kConnectSlice = 250ms;
        constexpr auto kAckTimeout = 1500ms;
        constexpr int64_t kSilenceBeforePingMs = 10000;
        constexpr int64_t kStaleAfterMs = 1500;
        constexpr auto kLaunchBackoff = 3s;
        constexpr auto kForeignServerBackoff = 10s;
        constexpr DWORD kExitOtherServer = 2;

        HANDLE CreateViewerJob()
        {
            HANDLE job = CreateJobObjectW(nullptr, nullptr);
            if (!job)
            {
                return nullptr;
            }
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)))
            {
                CloseHandle(job);
                return nullptr;
            }
            return job;
        }
    }

    ViewerManager::ViewerManager(std::function<void(Message const&)> onEvent) : m_onEvent(std::move(onEvent)) {}

    ViewerManager::~ViewerManager()
    {
        ResetConnection();
        if (m_job)
        {
            CloseHandle(m_job); // KILL_ON_JOB_CLOSE ends a Viewer that is still running
        }
    }

    int64_t ViewerManager::NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    }

    bool ViewerManager::ProcessExited() const
    {
        return m_process && WaitForSingleObject(m_process, 0) != WAIT_TIMEOUT;
    }

    bool ViewerManager::WaitProcessExit(std::chrono::milliseconds timeout) const
    {
        return !m_process || WaitForSingleObject(m_process, static_cast<DWORD>(timeout.count())) != WAIT_TIMEOUT;
    }

    bool ViewerManager::IsRunning() const
    {
        return m_process && m_conn && m_conn->Connected() && !ProcessExited();
    }

    bool ViewerManager::IsStale() const
    {
        return m_stale && m_pingSentMs != 0 && m_lastHeardMs.load() < m_pingSentMs;
    }

    void ViewerManager::TerminateNow()
    {
        if (m_job)
        {
            TerminateJobObject(m_job, 1);
        }
    }

    void ViewerManager::ResetConnection()
    {
        if (m_readerStop)
        {
            m_readerStop->store(true);
        }
        if (m_conn)
        {
            m_conn->Close();
        }
        if (m_reader.joinable())
        {
            m_reader.join();
        }
        {
            std::lock_guard lock(m_mutex);
            for (auto& entry : m_pending)
            {
                entry.second->done = true;
            }
            m_pending.clear();
        }
        m_cv.notify_all();
        m_conn.reset();
        m_readerEnded.reset();
        m_readerStop.reset();
        if (m_process)
        {
            CloseHandle(m_process);
            m_process = nullptr;
        }
        g_viewerPid.store(0);
        m_pingSentMs = 0;
        m_stale = false;
    }

    void ViewerManager::Kill(char const* reason)
    {
        FL_LOG_INFO(L"viewer kill reason={}", core::FromUtf8(reason));
        if (m_process)
        {
            TerminateProcess(m_process, 1);
            WaitForSingleObject(m_process, 2000); // the instance mutex is released only when the process is gone
        }
        ResetConnection();
    }

    bool ViewerManager::Launch()
    {
        if (!m_job)
        {
            m_job = CreateViewerJob();
            if (!m_job)
            {
                FL_LOG_INFO(L"viewer job creation failed {}", GetLastError());
            }
        }
        auto const pipe = core::ipc::PipeNameForCurrentUser();
        if (pipe.empty())
        {
            FL_LOG_INFO(L"viewer launch: pipe name unavailable");
            return false;
        }
        auto const dir = core::CurrentAppPaths().exeDir;
        auto const exe = (dir / L"FastLook.Viewer.exe").wstring();
        std::wstring cmd = std::format(L"\"{}\" --background --pipe {}", exe, pipe);
        if (!g_perfLogPath.empty())
        {
            cmd += std::format(L" --perf-log \"{}.viewer\"", g_perfLogPath);
        }

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, dir.c_str(), &si, &pi))
        {
            FL_LOG_INFO(L"viewer CreateProcess failed {}", GetLastError());
            return false;
        }
        if (m_job && !AssignProcessToJobObject(m_job, pi.hProcess))
        {
            FL_LOG_INFO(L"viewer job assign failed {}", GetLastError());
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        m_process = pi.hProcess;
        g_viewerPid.store(pi.dwProcessId);
        FL_LOG_INFO(L"viewer launched pid={}", pi.dwProcessId);
        return true;
    }

    bool ViewerManager::EnsureRunning()
    {
        if (IsRunning())
        {
            return true;
        }
        bool const hadPrevious = m_process || m_conn;
        if (hadPrevious)
        {
            // Disconnected while the process may still be in its exit sequence (it holds the instance mutex until it is gone).
            if (m_process && !WaitProcessExit(2000ms))
            {
                Kill("disconnected");
            }
            else
            {
                ResetConnection();
            }
        }
        if (Clock::now() < m_backoffUntil)
        {
            return false;
        }
        return Start(hadPrevious);
    }

    bool ViewerManager::Start(bool retryOnForeignExit)
    {
        if (!Launch())
        {
            m_backoffUntil = Clock::now() + kLaunchBackoff;
            return false;
        }

        auto const pipe = core::ipc::PipeNameForCurrentUser();
        std::optional<core::ipc::PipeConnection> conn;
        auto const deadline = Clock::now() + kConnectTimeout;
        while (!conn && Clock::now() < deadline && !ProcessExited())
        {
            conn = core::ipc::ConnectPipe(pipe, kConnectSlice, GetProcessId(m_process)); // only the Viewer we just started
        }

        if (ProcessExited())
        {
            // Exit code 2: another Viewer server holds the instance mutex / pipe name. Never spin on it.
            DWORD code = 0;
            GetExitCodeProcess(m_process, &code);
            FL_LOG_INFO(L"viewer exited at start code={}", code);
            ResetConnection();
            if (code == kExitOtherServer && retryOnForeignExit)
            {
                // Most likely our own previous Viewer still released its mutex late: one immediate retry, no back-off.
                return Start(false);
            }
            m_backoffUntil = Clock::now() + (code == kExitOtherServer ? kForeignServerBackoff : kLaunchBackoff);
            return false;
        }
        if (!conn)
        {
            FL_LOG_INFO(L"viewer connect timeout");
            m_backoffUntil = Clock::now() + kLaunchBackoff;
            Kill("connect timeout");
            return false;
        }

        m_conn = std::make_shared<core::ipc::PipeConnection>(std::move(*conn));
        m_readerEnded = std::make_shared<std::atomic<bool>>(false);
        m_readerStop = std::make_shared<std::atomic<bool>>(false);
        m_lastHeardMs.store(NowMs());
        m_pingSentMs = 0;
        m_stale = false;
        m_reader = std::thread([this, c = m_conn, e = m_readerEnded, s = m_readerStop] { ReaderLoop(c, e, s); });
        return true;
    }

    void ViewerManager::ReaderLoop(std::shared_ptr<core::ipc::PipeConnection> conn, std::shared_ptr<std::atomic<bool>> ended,
                                   std::shared_ptr<std::atomic<bool>> stop)
    {
        while (!stop->load())
        {
            auto message = conn->Receive(500ms);
            if (!message)
            {
                if (!conn->Connected())
                {
                    break;
                }
                continue;
            }
            m_lastHeardMs.store(NowMs());
            if (message->type == MsgType::Event)
            {
                if (m_onEvent)
                {
                    m_onEvent(*message);
                }
                continue;
            }
            if (message->replyTo != 0)
            {
                std::lock_guard lock(m_mutex);
                if (auto it = m_pending.find(message->replyTo); it != m_pending.end())
                {
                    it->second->reply = std::move(*message);
                    it->second->done = true;
                }
            }
            m_cv.notify_all();
        }
        {
            std::lock_guard lock(m_mutex);
            ended->store(true);
        }
        m_cv.notify_all();
    }

    std::optional<Message> ViewerManager::Request(Message msg, std::chrono::milliseconds timeout)
    {
        if (!m_conn || !m_conn->Connected())
        {
            return std::nullopt;
        }
        auto const pending = std::make_shared<Pending>();
        {
            std::lock_guard lock(m_mutex);
            m_pending[msg.id] = pending;
        }
        auto const deadline = Clock::now() + timeout;
        std::optional<Message> reply;
        if (m_conn->Send(msg, std::min<std::chrono::milliseconds>(timeout, 1000ms)))
        {
            std::unique_lock lock(m_mutex);
            auto const ended = m_readerEnded;
            m_cv.wait_until(lock, deadline, [&] { return pending->done || (ended && ended->load()); });
            reply = std::move(pending->reply);
        }
        std::lock_guard lock(m_mutex);
        m_pending.erase(msg.id);
        return reply;
    }

    std::optional<Message> ViewerManager::Deliver(Message msg)
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            if (ProcessExited())
            {
                ResetConnection(); // crash / idle exit
            }
            if (IsStale())
            {
                Kill("stale"); // heartbeat ping unanswered: restart before sending
            }
            if (!EnsureRunning())
            {
                FL_LOG_INFO(L"deliver failed: Viewer unavailable");
                return std::nullopt;
            }
            msg.id = NextId();
            if (auto ack = Request(msg, kAckTimeout))
            {
                return ack;
            }
            if (!m_conn->Connected() && WaitProcessExit(2000ms))
            {
                ResetConnection(); // the Viewer was in its exit sequence
                continue;
            }
            Kill("no ack");
        }
        FL_LOG_INFO(L"deliver failed");
        return std::nullopt;
    }

    void ViewerManager::Heartbeat()
    {
        if (!m_process)
        {
            return;
        }
        if (ProcessExited())
        {
            FL_LOG_INFO(L"viewer exited");
            ResetConnection();
            return;
        }
        if (!m_conn || !m_conn->Connected())
        {
            return;
        }
        int64_t const now = NowMs();
        int64_t const heard = m_lastHeardMs.load();
        if (m_pingSentMs != 0)
        {
            if (heard >= m_pingSentMs)
            {
                m_pingSentMs = 0;
                m_stale = false;
            }
            else if (!m_stale && now - m_pingSentMs > kStaleAfterMs)
            {
                m_stale = true;
                FL_LOG_INFO(L"viewer stale");
            }
            return;
        }
        if (now - heard >= kSilenceBeforePingMs)
        {
            Message ping;
            ping.type = MsgType::Ping;
            ping.id = NextId();
            m_conn->Send(ping, 200ms);
            m_pingSentMs = now;
        }
    }

    void ViewerManager::Shutdown(std::chrono::milliseconds grace)
    {
        if (m_process)
        {
            if (m_conn && m_conn->Connected())
            {
                Message bye;
                bye.type = MsgType::Shutdown;
                bye.id = NextId();
                m_conn->Send(bye, 200ms);
            }
            if (!WaitProcessExit(grace))
            {
                FL_LOG_INFO(L"viewer did not exit in time, terminating");
                TerminateProcess(m_process, 1);
                WaitForSingleObject(m_process, 500);
            }
        }
        ResetConnection();
    }
}
