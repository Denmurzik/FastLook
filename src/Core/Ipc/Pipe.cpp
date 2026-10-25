#include "Ipc/Pipe.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <utility>

#include "Ipc/Framing.h"

namespace fastlook::core::ipc
{
    using Clock = std::chrono::steady_clock;

    namespace
    {
        constexpr DWORD kBufferBytes = 64 * 1024;

        DWORD RemainingMs(Clock::time_point deadline)
        {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            return left <= 0 ? 0 : static_cast<DWORD>(std::min<long long>(left, 0x7FFFFFFF));
        }

        // Waits for an overlapped operation started on `pipe`. Returns true and the transferred byte count when it
        // completed successfully; false on failure, timeout (the operation is cancelled) or abort.
        bool FinishOverlapped(HANDLE pipe, OVERLAPPED& ov, bool startedOk, DWORD timeoutMs, DWORD& transferred, bool& timedOut)
        {
            timedOut = false;
            transferred = 0;
            if (!startedOk)
            {
                if (GetLastError() != ERROR_IO_PENDING)
                {
                    return false;
                }
                if (WaitForSingleObject(ov.hEvent, timeoutMs) == WAIT_TIMEOUT)
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
        std::atomic<bool> handleClosed{false};
        std::atomic<int> active{0};

        explicit Impl(HANDLE p) : pipe(p)
        {
            readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        }

        ~Impl()
        {
            CloseHandleOnce();
            CloseHandle(readEvent);
            CloseHandle(writeEvent);
        }

        void CloseHandleOnce()
        {
            if (!handleClosed.exchange(true))
            {
                CloseHandle(pipe);
            }
        }

        // Guards the handle against Close() racing with an in-flight Send/Receive: the handle is only closed when no
        // operation is active.
        struct Scope
        {
            Impl& impl;
            bool ok;
            explicit Scope(Impl& i) : impl(i)
            {
                impl.active.fetch_add(1);
                ok = !impl.closeRequested.load();
            }
            ~Scope()
            {
                if (impl.active.fetch_sub(1) == 1 && impl.closeRequested.load())
                {
                    impl.CloseHandleOnce();
                }
            }
        };

        void Close()
        {
            connected = false;
            closeRequested = true;
            if (active.load() == 0)
            {
                CloseHandleOnce();
            }
            else
            {
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

        auto deadline = Clock::now() + timeout;
        size_t written = 0;
        while (written < data.size())
        {
            OVERLAPPED ov{};
            ov.hEvent = m_impl->writeEvent;
            ResetEvent(ov.hEvent);
            auto chunk = static_cast<DWORD>(std::min<size_t>(data.size() - written, kBufferBytes));
            bool started = WriteFile(m_impl->pipe, data.data() + written, chunk, nullptr, &ov) != FALSE;
            DWORD n = 0;
            bool timedOut = false;
            if (!FinishOverlapped(m_impl->pipe, ov, started, RemainingMs(deadline), n, timedOut))
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

        auto deadline = Clock::now() + timeout;
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

            OVERLAPPED ov{};
            ov.hEvent = m_impl->readEvent;
            ResetEvent(ov.hEvent);
            bool started = ReadFile(m_impl->pipe, buf.data(), kBufferBytes, nullptr, &ov) != FALSE;
            DWORD n = 0;
            bool timedOut = false;
            if (!FinishOverlapped(m_impl->pipe, ov, started, RemainingMs(deadline), n, timedOut))
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

    struct PipeServer::Impl
    {
        std::wstring name;
        HANDLE pending = INVALID_HANDLE_VALUE; // created instance waiting for a client
        HANDLE cancelEvent = nullptr;

        HANDLE CreateInstance() const
        {
            return CreateNamedPipeW(name.c_str(),
                                    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                    kBufferBytes, kBufferBytes, 0, nullptr);
        }
    };

    PipeServer::PipeServer(std::wstring name) : m_impl(std::make_unique<Impl>())
    {
        m_impl->name = std::move(name);
        m_impl->cancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_impl->pending = m_impl->CreateInstance();
    }

    PipeServer::~PipeServer()
    {
        if (m_impl->pending != INVALID_HANDLE_VALUE)
        {
            CloseHandle(m_impl->pending);
        }
        CloseHandle(m_impl->cancelEvent);
    }

    void PipeServer::Cancel()
    {
        SetEvent(m_impl->cancelEvent);
    }

    std::optional<PipeConnection> PipeServer::Accept(std::chrono::milliseconds timeout)
    {
        auto deadline = Clock::now() + timeout;
        auto cancelled = [&] { return WaitForSingleObject(m_impl->cancelEvent, 0) == WAIT_OBJECT_0; };

        // With a single instance the next one can only be created after the previous connection is closed.
        while (m_impl->pending == INVALID_HANDLE_VALUE)
        {
            if (cancelled())
            {
                return std::nullopt;
            }
            m_impl->pending = m_impl->CreateInstance();
            if (m_impl->pending != INVALID_HANDLE_VALUE)
            {
                break;
            }
            DWORD left = RemainingMs(deadline);
            if (left == 0)
            {
                return std::nullopt;
            }
            if (WaitForSingleObject(m_impl->cancelEvent, std::min<DWORD>(left, 20)) == WAIT_OBJECT_0)
            {
                return std::nullopt;
            }
        }
        if (cancelled())
        {
            return std::nullopt;
        }

        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        OVERLAPPED ov{};
        ov.hEvent = event;
        bool connected = false;
        if (ConnectNamedPipe(m_impl->pending, &ov))
        {
            connected = true;
        }
        else
        {
            switch (GetLastError())
            {
            case ERROR_PIPE_CONNECTED:
                connected = true;
                break;
            case ERROR_IO_PENDING:
            {
                HANDLE handles[] = {event, m_impl->cancelEvent};
                DWORD r = WaitForMultipleObjects(2, handles, FALSE, RemainingMs(deadline));
                if (r != WAIT_OBJECT_0)
                {
                    CancelIoEx(m_impl->pending, &ov);
                }
                DWORD ignored = 0;
                // A client that connected just before the cancel still counts.
                connected = GetOverlappedResult(m_impl->pending, &ov, &ignored, TRUE) != FALSE;
                break;
            }
            default:
                break;
            }
        }
        CloseHandle(event);

        if (!connected)
        {
            return std::nullopt;
        }
        HANDLE h = std::exchange(m_impl->pending, INVALID_HANDLE_VALUE);
        return PipeConnection(std::make_unique<PipeConnection::Impl>(h));
    }

    std::optional<PipeConnection> ConnectPipe(std::wstring const& name, std::chrono::milliseconds timeout)
    {
        auto deadline = Clock::now() + timeout;
        for (;;)
        {
            HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                return PipeConnection(std::make_unique<PipeConnection::Impl>(h));
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
