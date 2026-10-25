#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "Ipc/Pipe.h"
#include "Ipc/Protocol.h"

namespace fastlook::daemon
{
    // Owns the Viewer process (server mode) and its pipe connection. Everything except the reader thread's own work runs on
    // the controller thread.
    class ViewerManager
    {
    public:
        explicit ViewerManager(std::function<void(core::ipc::Message const&)> onEvent);
        ~ViewerManager();
        ViewerManager(ViewerManager const&) = delete;
        ViewerManager& operator=(ViewerManager const&) = delete;

        // Starts the Viewer (`--background`, under a Job object) when it is not running and connects to its pipe.
        // After a failed start it returns false at once until a back-off period has passed.
        bool EnsureRunning();
        // Decision 53: no ping on the hot path; the request goes out at once and its ack is awaited (<= 1500 ms).
        // No ack / a disconnect restarts the Viewer and resends once.
        std::optional<core::ipc::Message> Deliver(core::ipc::Message msg);
        // Waits for the reply with replyTo == msg.id; nullopt at once on a disconnect.
        std::optional<core::ipc::Message> Request(core::ipc::Message msg, std::chrono::milliseconds timeout);
        // Called every second: reaps an exited process, pings after 10 s of silence, marks an unanswered ping stale.
        void Heartbeat();
        bool IsRunning() const;
        bool IsStale() const;
        // `shutdown`, waits `grace` for the process, then terminates it.
        void Shutdown(std::chrono::milliseconds grace = std::chrono::milliseconds(2000));

    private:
        using Clock = std::chrono::steady_clock;

        struct Pending
        {
            bool done = false;
            std::optional<core::ipc::Message> reply;
        };

        bool Launch();
        void ResetConnection();
        void Kill(char const* reason);
        bool ProcessExited() const;
        bool WaitProcessExit(std::chrono::milliseconds timeout) const;
        uint32_t NextId() { return ++m_nextId; }
        void ReaderLoop(std::shared_ptr<core::ipc::PipeConnection> conn, std::shared_ptr<std::atomic<bool>> ended,
                        std::shared_ptr<std::atomic<bool>> stop);
        static int64_t NowMs();

        std::function<void(core::ipc::Message const&)> m_onEvent;
        HANDLE m_job = nullptr;
        HANDLE m_process = nullptr;
        std::shared_ptr<core::ipc::PipeConnection> m_conn;
        std::thread m_reader;
        std::shared_ptr<std::atomic<bool>> m_readerEnded;
        std::shared_ptr<std::atomic<bool>> m_readerStop;

        std::mutex m_mutex; // guards m_pending and the reader's `ended` flag
        std::condition_variable m_cv;
        std::map<uint32_t, std::shared_ptr<Pending>> m_pending;

        std::atomic<int64_t> m_lastHeardMs{0};
        int64_t m_pingSentMs = 0; // 0 = no ping outstanding
        bool m_stale = false;
        uint32_t m_nextId = 0;
        Clock::time_point m_backoffUntil{};
    };
}
