#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <winrt/Microsoft.UI.Dispatching.h>

#include "Ipc/Protocol.h"

namespace fastlook::viewer
{
    // Serves the daemon's pipe. The server threads only move bytes: every request is handled by `onUiThread` on the UI
    // thread (so a hung UI thread produces no pong/ack) and the reply is written by the server's writer thread. Replies
    // get a fresh `id` and `replyTo` = the request id here; the handler fills in the type and body only. A request that
    // is handled after Stop() is dropped without a reply.
    class IpcServer
    {
    public:
        using Handler = std::function<std::optional<core::ipc::Message>(core::ipc::Message const&)>;

        IpcServer(std::wstring pipeName, winrt::Microsoft::UI::Dispatching::DispatcherQueue ui, Handler onUiThread);
        ~IpcServer();
        IpcServer(IpcServer const&) = delete;
        IpcServer& operator=(IpcServer const&) = delete;

        // Creates the pipe instance on the calling thread, then starts the accept/read/write threads. A pipe name held
        // by another process is logged and retried with back-off; it never fails Start.
        void Start();
        // Cancels the pending accept, closes the connection, drops queued replies and joins the threads. Idempotent.
        void Stop();
        // Queues an unsolicited `event` for the connected daemon; dropped when nobody is connected.
        void SendEvent(std::string_view name);

    private:
        struct State;
        std::shared_ptr<State> m_state;
    };
}
