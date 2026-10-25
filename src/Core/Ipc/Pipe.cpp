#include "Ipc/Pipe.h"

#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <atomic>
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

        // TOKEN_USER of `token` into `buf`.
        bool QueryTokenUser(HANDLE token, std::vector<char>& buf)
        {
            DWORD size = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &size);
            if (size == 0)
            {
                return false;
            }
            buf.assign(size, 0);
            return GetTokenInformation(token, TokenUser, buf.data(), size, &size) != FALSE;
        }

        PSID TokenUserSid(std::vector<char>& buf)
        {
            return reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
        }

        bool CurrentUserSid(std::vector<char>& buf)
        {
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            {
                return false;
            }
            bool ok = QueryTokenUser(token, buf);
            CloseHandle(token);
            return ok;
        }

        // True when the process serving `pipe` runs as the current user.
        bool ServerIsCurrentUser(HANDLE pipe)
        {
            ULONG pid = 0;
            if (!GetNamedPipeServerProcessId(pipe, &pid))
            {
                return false;
            }
            std::vector<char> mine;
            if (!CurrentUserSid(mine))
            {
                return false;
            }
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!process)
            {
                return false;
            }
            HANDLE token = nullptr;
            bool same = false;
            if (OpenProcessToken(process, TOKEN_QUERY, &token))
            {
                std::vector<char> theirs;
                same = QueryTokenUser(token, theirs) && EqualSid(TokenUserSid(mine), TokenUserSid(theirs));
                CloseHandle(token);
            }
            CloseHandle(process);
            return same;
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

        explicit Impl(HANDLE p) : pipe(p)
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
        std::optional<PipeConnection> Wrap(HANDLE h)
        {
            auto impl = std::make_unique<PipeConnection::Impl>(h);
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
        HANDLE pending = INVALID_HANDLE_VALUE; // created instance waiting for a client
        HANDLE cancelEvent = nullptr;
        bool firstCreated = false;
        std::atomic<DWORD> error{0};

        // The first instance claims the name (FIRST_PIPE_INSTANCE); later ones are created after the previous
        // connection closed and report ERROR_PIPE_BUSY while it is still open. Only the current user and SYSTEM
        // may open the pipe.
        HANDLE CreateInstance() const
        {
            std::vector<char> user;
            LPWSTR sidString = nullptr;
            if (!CurrentUserSid(user) || !ConvertSidToStringSidW(TokenUserSid(user), &sidString))
            {
                SetLastError(ERROR_NO_TOKEN);
                return INVALID_HANDLE_VALUE;
            }
            std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sidString + L")(A;;GA;;;SY)";
            LocalFree(sidString);

            SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr))
            {
                return INVALID_HANDLE_VALUE;
            }
            DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (firstCreated ? 0 : FILE_FLAG_FIRST_PIPE_INSTANCE);
            HANDLE h = CreateNamedPipeW(name.c_str(), openMode,
                                        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, kBufferBytes,
                                        kBufferBytes, 0, &sa);
            DWORD err = GetLastError();
            LocalFree(sa.lpSecurityDescriptor);
            SetLastError(err);
            return h;
        }

        bool Cancelled() const { return WaitForSingleObject(cancelEvent, 0) == WAIT_OBJECT_0; }

        // Makes sure `pending` is a created instance. False on cancel, timeout or a creation error (recorded in `error`).
        bool EnsurePending(Clock::time_point deadline)
        {
            while (pending == INVALID_HANDLE_VALUE)
            {
                if (Cancelled())
                {
                    return false;
                }
                pending = CreateInstance();
                if (pending != INVALID_HANDLE_VALUE)
                {
                    firstCreated = true;
                    error = 0;
                    return true;
                }
                DWORD err = GetLastError();
                if (err != ERROR_PIPE_BUSY || !firstCreated)
                {
                    error = err;
                    return false;
                }
                DWORD left = RemainingMs(deadline);
                if (left == 0 || WaitForSingleObject(cancelEvent, std::min<DWORD>(left, 5)) == WAIT_OBJECT_0)
                {
                    return false;
                }
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
        m_impl->cancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_impl->cancelEvent)
        {
            m_impl->error = GetLastError();
            return;
        }
        m_impl->pending = m_impl->CreateInstance();
        if (m_impl->pending == INVALID_HANDLE_VALUE)
        {
            m_impl->error = GetLastError();
        }
        else
        {
            m_impl->firstCreated = true;
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
                return Wrap(std::exchange(m_impl->pending, INVALID_HANDLE_VALUE));
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

    std::optional<PipeConnection> ConnectPipe(std::wstring const& name, std::chrono::milliseconds timeout)
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
                if (!ServerIsCurrentUser(h))
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
