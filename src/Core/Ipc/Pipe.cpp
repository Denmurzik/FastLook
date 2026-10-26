#include "Ipc/Pipe.h"

#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "Ipc/Framing.h"

namespace fastlook::core::ipc
{
    using Clock = std::chrono::steady_clock;

    namespace
    {
        constexpr DWORD kBufferBytes = 64 * 1024;

        Clock::time_point DeadlineAfter(std::chrono::milliseconds timeout)
        {
            // Clamp so that now() + timeout cannot overflow (callers may pass milliseconds::max()).
            constexpr std::chrono::milliseconds kMax = std::chrono::hours(24 * 365);
            return Clock::now() + std::clamp(timeout, std::chrono::milliseconds(0), kMax);
        }

        DWORD RemainingMs(Clock::time_point deadline)
        {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            return left <= 0 ? 0 : static_cast<DWORD>(std::min<long long>(left, 0x7FFFFFFF));
        }

        // True when the process at the other end of `pipe` (queried by `pidOf`) runs as `user`.
        template <typename PidQuery>
        bool PeerIsUser(HANDLE pipe, PidQuery pidOf, std::wstring const& user)
        {
            ULONG pid = 0;
            if (user.empty() || !pidOf(pipe, &pid))
            {
                return false;
            }
            return UserSidString(pid) == user;
        }

        // Waits for an overlapped operation started on `pipe`. Returns true and the transferred byte count when it
        // completed successfully; false on failure, timeout (the operation is cancelled) or abort.
        // `closeRequested` closes the window where Close() ran CancelIoEx before this operation was issued.
        bool FinishOverlapped(HANDLE pipe, OVERLAPPED& ov, bool startedOk, DWORD timeoutMs, std::atomic<bool> const& closeRequested,
                              DWORD& transferred, bool& timedOut)
        {
            timedOut = false;
            transferred = 0;
            if (!startedOk)
            {
                if (GetLastError() != ERROR_IO_PENDING)
                {
                    return false;
                }
                if (closeRequested.load())
                {
                    CancelIoEx(pipe, &ov);
                }
                else if (WaitForSingleObject(ov.hEvent, timeoutMs) == WAIT_TIMEOUT)
                {
                    timedOut = true;
                    CancelIoEx(pipe, &ov);
                }
            }
            // After a cancel this still reports data that completed before the cancel took effect.
            if (!GetOverlappedResult(pipe, &ov, &transferred, TRUE))
            {
                return false;
            }
            timedOut = false;
            return true;
        }
    }

    struct PipeConnection::Impl
    {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        HANDLE readEvent = nullptr;
        HANDLE writeEvent = nullptr;
        FrameReader reader;
        std::atomic<bool> connected{true};
        std::atomic<bool> closeRequested{false};
        // Guards `active`, the close decision and the handle value against Close() racing with Send/Receive.
        std::mutex guard;
        int active = 0;
        bool handleClosed = false;
        // Cleared once the handle is really closed; lets the server know when it may create the next instance.
        std::shared_ptr<std::atomic<bool>> alive;

        explicit Impl(HANDLE p, std::shared_ptr<std::atomic<bool>> aliveFlag = nullptr) : pipe(p), alive(std::move(aliveFlag))
        {
            readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        }

        ~Impl()
        {
            CloseHandleLocked();
            if (readEvent)
            {
                CloseHandle(readEvent);
            }
            if (writeEvent)
            {
                CloseHandle(writeEvent);
            }
        }

        bool Valid() const { return readEvent && writeEvent; }

        void CloseHandleLocked()
        {
            if (!handleClosed)
            {
                handleClosed = true;
                CloseHandle(pipe);
                if (alive)
                {
                    alive->store(false);
                }
            }
        }

        // Keeps the handle open while an operation is active: Close() then only cancels, and the last operation to
        // finish closes the handle.
        struct Scope
        {
            Impl& impl;
            bool ok;
            explicit Scope(Impl& i) : impl(i)
            {
                std::lock_guard lock(impl.guard);
                ++impl.active;
                ok = !impl.closeRequested.load();
            }
            ~Scope()
            {
                std::lock_guard lock(impl.guard);
                if (--impl.active == 0 && impl.closeRequested.load())
                {
                    impl.CloseHandleLocked();
                }
            }
        };

        void Close()
        {
            std::lock_guard lock(guard);
            connected = false;
            closeRequested = true;
            if (active == 0)
            {
                CloseHandleLocked();
            }
            else
            {
                // The handle stays valid under the lock while an operation is active.
                CancelIoEx(pipe, nullptr);
            }
        }
    };

    PipeConnection::PipeConnection(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
    PipeConnection::PipeConnection(PipeConnection&&) noexcept = default;
    PipeConnection& PipeConnection::operator=(PipeConnection&&) noexcept = default;
    PipeConnection::~PipeConnection() = default;

    bool PipeConnection::Connected() const
    {
        return m_impl && m_impl->connected.load();
    }

    void PipeConnection::Close()
    {
        if (m_impl)
        {
            m_impl->Close();
        }
    }

    bool PipeConnection::Send(Message const& message, std::chrono::milliseconds timeout)
    {
        if (!m_impl)
        {
            return false;
        }
        Impl::Scope scope(*m_impl);
        if (!scope.ok || !m_impl->connected)
        {
            return false;
        }

        std::string payload = Serialize(message);
        if (payload.size() > kMaxFrameBytes)
        {
            return false;
        }
        std::string data = Frame(payload);

        auto deadline = DeadlineAfter(timeout);
        size_t written = 0;
        while (written < data.size())
        {
            if (m_impl->closeRequested.load())
            {
                return false;
            }
            OVERLAPPED ov{};
            ov.hEvent = m_impl->writeEvent;
            ResetEvent(ov.hEvent);
            auto chunk = static_cast<DWORD>(std::min<size_t>(data.size() - written, kBufferBytes));
            bool started = WriteFile(m_impl->pipe, data.data() + written, chunk, nullptr, &ov) != FALSE;
            DWORD n = 0;
            bool timedOut = false;
            if (!FinishOverlapped(m_impl->pipe, ov, started, RemainingMs(deadline), m_impl->closeRequested, n, timedOut))
            {
                // A partial frame on the wire corrupts the stream, so any failure ends the connection.
                m_impl->connected = false;
                return false;
            }
            written += n;
        }
        return true;
    }

    std::optional<Message> PipeConnection::Receive(std::chrono::milliseconds timeout)
    {
        if (!m_impl)
        {
            return std::nullopt;
        }
        Impl::Scope scope(*m_impl);
        if (!scope.ok || !m_impl->connected)
        {
            return std::nullopt;
        }

        auto deadline = DeadlineAfter(timeout);
        std::array<char, kBufferBytes> buf;
        for (;;)
        {
            if (auto frame = m_impl->reader.Next())
            {
                auto message = Parse(*frame);
                if (!message)
                {
                    m_impl->connected = false;
                }
                return message;
            }
            if (m_impl->reader.Failed())
            {
                m_impl->connected = false;
                return std::nullopt;
            }

            if (m_impl->closeRequested.load())
            {
                return std::nullopt;
            }
            OVERLAPPED ov{};
            ov.hEvent = m_impl->readEvent;
            ResetEvent(ov.hEvent);
            bool started = ReadFile(m_impl->pipe, buf.data(), kBufferBytes, nullptr, &ov) != FALSE;
            DWORD n = 0;
            bool timedOut = false;
            if (!FinishOverlapped(m_impl->pipe, ov, started, RemainingMs(deadline), m_impl->closeRequested, n, timedOut))
            {
                if (!timedOut)
                {
                    m_impl->connected = false; // broken pipe, aborted by Close(), or another hard error
                }
                return std::nullopt;
            }
            m_impl->reader.Feed({buf.data(), n});
        }
    }

    namespace
    {
        // Wraps a freshly opened/accepted handle; closes it and returns nullopt when the events cannot be created.
        std::optional<PipeConnection> Wrap(HANDLE h, std::shared_ptr<std::atomic<bool>> alive = nullptr)
        {
            auto impl = std::make_unique<PipeConnection::Impl>(h, std::move(alive));
            if (!impl->Valid())
            {
                return std::nullopt; // ~Impl closes the handle
            }
            return PipeConnection(std::move(impl));
        }
    }

    struct PipeServer::Impl
    {
        std::wstring name;
        std::wstring user;                     // SID string of the current user
        PSECURITY_DESCRIPTOR descriptor = nullptr; // built once: current user and SYSTEM only
        HANDLE pending = INVALID_HANDLE_VALUE; // created instance waiting for a client
        HANDLE cancelEvent = nullptr;
        std::shared_ptr<std::atomic<bool>> current; // liveness of the connection handed out last
        std::atomic<DWORD> error{0};

        ~Impl()
        {
            if (descriptor)
            {
                LocalFree(descriptor);
            }
        }

        bool Init()
        {
            user = UserSidString(0);
            if (user.empty())
            {
                error = ERROR_NO_TOKEN;
                return false;
            }
            std::wstring sddl = L"D:P(A;;GA;;;" + user + L")(A;;GA;;;SY)";
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            {
                error = GetLastError();
                return false;
            }
            cancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!cancelEvent)
            {
                error = GetLastError();
                return false;
            }
            return true;
        }

        // Every instance claims the name with FIRST_PIPE_INSTANCE: if anyone else holds it the call fails with
        // ERROR_ACCESS_DENIED instead of silently joining a pipe whose DACL/instance limit we do not control.
        HANDLE CreateInstance() const
        {
            SECURITY_ATTRIBUTES sa{sizeof(sa), descriptor, FALSE};
            return CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, kBufferBytes,
                                    kBufferBytes, 0, &sa);
        }

        bool Cancelled() const { return WaitForSingleObject(cancelEvent, 0) == WAIT_OBJECT_0; }

        // Makes sure `pending` is a created instance. The server serves one connection at a time, so while the
        // previous one is alive this waits (it cannot create the next instance yet). False on cancel, timeout or a
        // creation error (recorded in `error`).
        bool EnsurePending(Clock::time_point deadline)
        {
            while (pending == INVALID_HANDLE_VALUE)
            {
                if (Cancelled())
                {
                    return false;
                }
                if (current && current->load())
                {
                    DWORD left = RemainingMs(deadline);
                    if (left == 0 || WaitForSingleObject(cancelEvent, std::min<DWORD>(left, 20)) == WAIT_OBJECT_0)
                    {
                        return false;
                    }
                    continue;
                }
                pending = CreateInstance();
                if (pending == INVALID_HANDLE_VALUE)
                {
                    error = GetLastError(); // ACCESS_DENIED / PIPE_BUSY: somebody else holds the name
                    return false;
                }
                error = 0;
            }
            return true;
        }

        void DiscardPending()
        {
            DisconnectNamedPipe(pending);
            CloseHandle(pending);
            pending = INVALID_HANDLE_VALUE;
        }
    };

    PipeServer::PipeServer(std::wstring name) : m_impl(std::make_unique<Impl>())
    {
        m_impl->name = std::move(name);
        if (m_impl->Init())
        {
            m_impl->pending = m_impl->CreateInstance();
            if (m_impl->pending == INVALID_HANDLE_VALUE)
            {
                m_impl->error = GetLastError();
            }
        }
    }

    PipeServer::~PipeServer()
    {
        if (m_impl->pending != INVALID_HANDLE_VALUE)
        {
            CloseHandle(m_impl->pending);
        }
        if (m_impl->cancelEvent)
        {
            CloseHandle(m_impl->cancelEvent);
        }
    }

    unsigned long PipeServer::Error() const
    {
        return m_impl->error.load();
    }

    void PipeServer::Cancel()
    {
        if (m_impl->cancelEvent)
        {
            SetEvent(m_impl->cancelEvent);
        }
    }

    std::optional<PipeConnection> PipeServer::Accept(std::chrono::milliseconds timeout)
    {
        if (!m_impl->cancelEvent)
        {
            return std::nullopt;
        }
        auto deadline = DeadlineAfter(timeout);

        for (;;)
        {
            if (!m_impl->EnsurePending(deadline) || m_impl->Cancelled())
            {
                return std::nullopt;
            }

            HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!event)
            {
                m_impl->error = GetLastError();
                return std::nullopt;
            }
            OVERLAPPED ov{};
            ov.hEvent = event;
            enum class Outcome { Connected, Stop, ClientGone, Failed } outcome = Outcome::Failed;
            DWORD err = 0;
            if (ConnectNamedPipe(m_impl->pending, &ov))
            {
                outcome = Outcome::Connected;
            }
            else
            {
                err = GetLastError();
                if (err == ERROR_PIPE_CONNECTED)
                {
                    outcome = Outcome::Connected;
                }
                else if (err == ERROR_IO_PENDING)
                {
                    HANDLE handles[] = {event, m_impl->cancelEvent};
                    DWORD r = WaitForMultipleObjects(2, handles, FALSE, RemainingMs(deadline));
                    bool stop = r != WAIT_OBJECT_0;
                    if (stop)
                    {
                        CancelIoEx(m_impl->pending, &ov);
                    }
                    DWORD ignored = 0;
                    // A client that connected just before the cancel still counts.
                    if (GetOverlappedResult(m_impl->pending, &ov, &ignored, TRUE))
                    {
                        outcome = Outcome::Connected;
                    }
                    else
                    {
                        err = GetLastError();
                        outcome = (stop || err == ERROR_OPERATION_ABORTED) ? Outcome::Stop : Outcome::Failed;
                    }
                }
            }
            CloseHandle(event);

            if (outcome == Outcome::Failed && (err == ERROR_NO_DATA || err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED))
            {
                outcome = Outcome::ClientGone;
            }
            switch (outcome)
            {
            case Outcome::Connected:
            {
                auto pidOf = [](HANDLE pipe, ULONG* pid) { return GetNamedPipeClientProcessId(pipe, pid) != FALSE; };
                if (!PeerIsUser(m_impl->pending, pidOf, m_impl->user))
                {
                    // Not our user (or already gone): drop it and keep waiting for a real client.
                    m_impl->DiscardPending();
                    if (RemainingMs(deadline) == 0)
                    {
                        return std::nullopt;
                    }
                    break;
                }
                auto alive = std::make_shared<std::atomic<bool>>(true);
                auto connection = Wrap(std::exchange(m_impl->pending, INVALID_HANDLE_VALUE), alive);
                if (connection)
                {
                    m_impl->current = std::move(alive);
                }
                return connection;
            }
            case Outcome::Stop:
                return std::nullopt;
            case Outcome::ClientGone:
                // The client vanished before we accepted it; this instance is unusable, so replace it.
                m_impl->DiscardPending();
                if (RemainingMs(deadline) == 0)
                {
                    return std::nullopt;
                }
                break;
            case Outcome::Failed:
                m_impl->error = err;
                m_impl->DiscardPending();
                return std::nullopt;
            }
        }
    }

    std::optional<PipeConnection> ConnectPipe(std::wstring const& name, std::chrono::milliseconds timeout, unsigned long expectedServerPid)
    {
        if (name.empty())
        {
            return std::nullopt;
        }
        auto deadline = DeadlineAfter(timeout);
        for (;;)
        {
            HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                auto pidOf = [](HANDLE pipe, ULONG* pid) { return GetNamedPipeServerProcessId(pipe, pid) != FALSE; };
                ULONG serverPid = 0;
                if (!PeerIsUser(h, pidOf, UserSidString(0)) ||
                    (expectedServerPid != 0 && (!pidOf(h, &serverPid) || serverPid != expectedServerPid)))
                {
                    CloseHandle(h);
                    return std::nullopt;
                }
                return Wrap(h);
            }
            DWORD err = GetLastError();
            DWORD left = RemainingMs(deadline);
            if (left == 0 || (err != ERROR_FILE_NOT_FOUND && err != ERROR_PIPE_BUSY))
            {
                return std::nullopt;
            }
            if (err == ERROR_PIPE_BUSY)
            {
                WaitNamedPipeW(name.c_str(), std::min<DWORD>(left, 50));
            }
            else
            {
                Sleep(std::min<DWORD>(left, 10));
            }
        }
    }
}
