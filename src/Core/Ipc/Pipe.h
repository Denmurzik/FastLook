#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "Ipc/Protocol.h"

namespace fastlook::core::ipc
{
    // Move-only. One reader thread and one writer thread may use it concurrently; Close() may be called from any thread.
    class PipeConnection
    {
    public:
        struct Impl;
        explicit PipeConnection(std::unique_ptr<Impl> impl);
        PipeConnection(PipeConnection&&) noexcept;
        PipeConnection& operator=(PipeConnection&&) noexcept;
        ~PipeConnection();

        bool Send(Message const&, std::chrono::milliseconds timeout);
        // nullopt: timeout, disconnect, bad frame or malformed message (the last three also disconnect).
        std::optional<Message> Receive(std::chrono::milliseconds timeout);
        bool Connected() const;
        void Close();

    private:
        std::unique_ptr<Impl> m_impl;
    };

    // Single-instance server (PIPE_REJECT_REMOTE_CLIENTS, overlapped). The next client is accepted once the
    // previous connection is closed.
    class PipeServer
    {
    public:
        explicit PipeServer(std::wstring name);
        ~PipeServer();
        PipeServer(PipeServer const&) = delete;
        PipeServer& operator=(PipeServer const&) = delete;

        std::optional<PipeConnection> Accept(std::chrono::milliseconds timeout);
        // Wakes a blocked Accept and makes every later Accept return nullopt.
        void Cancel();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

    // Retries until timeout while the server does not exist yet or is busy.
    std::optional<PipeConnection> ConnectPipe(std::wstring const& name, std::chrono::milliseconds timeout);
}
