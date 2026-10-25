#include "pch.h"
#include "Ipc/IpcServer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "Diagnostics/Log.h"
#include "Ipc/Pipe.h"
#include "Text/Utf.h"

namespace fastlook::viewer
{
    using core::ipc::Message;
    using core::ipc::MsgType;
    using core::ipc::PipeConnection;
    using namespace std::chrono_literals;

    namespace
    {
        constexpr std::chrono::milliseconds kSendTimeout = 1500ms;
        constexpr std::chrono::milliseconds kReceiveSlice = std::chrono::hours(1);
        constexpr std::chrono::milliseconds kAcceptSlice = 500ms;
        constexpr std::chrono::milliseconds kBackoffMin = 250ms;
        constexpr std::chrono::milliseconds kBackoffMax = 2000ms;
    }

    struct IpcServer::State : std::enable_shared_from_this<State>
    {
        struct Outgoing
        {
            std::shared_ptr<PipeConnection> connection;
            Message message;
        };

        std::wstring pipeName;
        winrt::Microsoft::UI::Dispatching::DispatcherQueue ui{nullptr};
        Handler handler;

        std::mutex mutex; // guards the members below except `exiting`
        std::condition_variable wake;
        std::unique_ptr<core::ipc::PipeServer> server;
        std::shared_ptr<PipeConnection> connection;
        std::deque<Outgoing> outgoing;
        uint32_t nextId = 0;
        bool started = false;
        std::thread acceptThread, writerThread;
        std::atomic<bool> exiting{false};

        void Enqueue(std::shared_ptr<PipeConnection> target, Message message)
        {
            if (!target)
            {
                return;
            }
            {
                std::lock_guard lock(mutex);
                if (exiting.load())
                {
                    return;
                }
                message.id = ++nextId;
                outgoing.push_back({std::move(target), std::move(message)});
            }
            wake.notify_all();
        }

        // Runs on the UI thread.
        void Dispatch(std::shared_ptr<PipeConnection> const& from, Message const& request)
        {
            if (exiting.load())
            {
                return; // shutting down: no ack, the daemon relaunches (decision 53)
            }
            std::optional<Message> reply;
            try
            {
                reply = handler(request);
            }
            catch (winrt::hresult_error const& e)
            {
                FL_LOG_INFO(L"IPC request handler failed: {}", std::wstring(e.message()));
                return;
            }
            catch (std::exception const& e)
            {
                FL_LOG_INFO(L"IPC request handler failed: {}", core::FromUtf8(e.what()));
                return;
            }
            catch (...)
            {
                FL_LOG_INFO(L"IPC request handler failed");
                return;
            }
            if (!reply || exiting.load())
            {
                return;
            }
            reply->replyTo = request.id;
            Enqueue(from, std::move(*reply));
        }

        void WriterLoop()
        {
            for (;;)
            {
                Outgoing item;
                {
                    std::unique_lock lock(mutex);
                    wake.wait(lock, [this] { return exiting.load() || !outgoing.empty(); });
                    if (exiting.load())
                    {
                        return;
                    }
                    item = std::move(outgoing.front());
                    outgoing.pop_front();
                }
                if (!item.connection->Send(item.message, kSendTimeout))
                {
                    item.connection->Close(); // a failed or partial write corrupts the stream
                }
            }
        }

        void Serve(std::shared_ptr<PipeConnection> const& conn)
        {
            {
                std::lock_guard lock(mutex);
                connection = conn;
            }
            while (!exiting.load() && conn->Connected())
            {
                auto request = conn->Receive(kReceiveSlice);
                if (!request)
                {
                    continue; // a timeout slice: Connected() decides whether the loop goes on
                }
                switch (request->type)
                {
                case MsgType::Show:
                case MsgType::Toggle:
                case MsgType::Items:
                case MsgType::Ping:
                case MsgType::SettingsChanged:
                case MsgType::OpenSettings:
                case MsgType::Shutdown:
                    break;
                default:
                    continue; // replies and events are for the daemon, not for us
                }
                bool const queued = ui.TryEnqueue([self = shared_from_this(), conn, message = std::move(*request)] {
                    self->Dispatch(conn, message);
                });
                if (!queued)
                {
                    break; // the dispatcher is shutting down
                }
            }
            conn->Close();
            std::lock_guard lock(mutex);
            if (connection == conn)
            {
                connection.reset();
            }
        }

        void AcceptLoop()
        {
            auto backoff = kBackoffMin;
            DWORD loggedError = 0;
            while (!exiting.load())
            {
                std::optional<PipeConnection> accepted = server->Accept(kAcceptSlice);
                if (exiting.load())
                {
                    return;
                }
                if (accepted)
                {
                    backoff = kBackoffMin;
                    Serve(std::make_shared<PipeConnection>(std::move(*accepted)));
                    continue;
                }
                if (DWORD const error = server->Error(); error != 0)
                {
                    // Somebody else holds the pipe name (or the instance cannot be created): never spin on Accept.
                    if (error != loggedError)
                    {
                        loggedError = error;
                        FL_LOG_INFO(L"IPC pipe unavailable, error {}; retrying", error);
                    }
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, backoff, [this] { return exiting.load(); });
                    backoff = std::min(backoff * 2, kBackoffMax);
                    continue;
                }
                if (loggedError != 0)
                {
                    loggedError = 0;
                    FL_LOG_INFO(L"IPC pipe available again");
                }
                backoff = kBackoffMin;
            }
        }
    };

    IpcServer::IpcServer(std::wstring pipeName, winrt::Microsoft::UI::Dispatching::DispatcherQueue ui, Handler onUiThread)
        : m_state(std::make_shared<State>())
    {
        m_state->pipeName = std::move(pipeName);
        m_state->ui = std::move(ui);
        m_state->handler = std::move(onUiThread);
    }

    IpcServer::~IpcServer()
    {
        Stop();
    }

    void IpcServer::Start()
    {
        auto& s = *m_state;
        std::lock_guard lock(s.mutex);
        if (s.started)
        {
            return;
        }
        s.started = true;
        s.server = std::make_unique<core::ipc::PipeServer>(s.pipeName);
        if (DWORD const error = s.server->Error(); error != 0)
        {
            FL_LOG_INFO(L"IPC pipe could not be created, error {}; retrying", error);
        }
        s.writerThread = std::thread([state = m_state] { state->WriterLoop(); });
        s.acceptThread = std::thread([state = m_state] { state->AcceptLoop(); });
    }

    void IpcServer::Stop()
    {
        auto& s = *m_state;
        std::shared_ptr<PipeConnection> connection;
        {
            std::lock_guard lock(s.mutex);
            s.exiting = true;
            s.outgoing.clear();
            connection = s.connection;
            if (s.server)
            {
                s.server->Cancel();
            }
        }
        s.wake.notify_all();
        if (connection)
        {
            connection->Close();
        }
        // The threads never wait for the UI thread, so joining here (on the UI thread) cannot deadlock.
        if (s.acceptThread.joinable())
        {
            s.acceptThread.join();
        }
        if (s.writerThread.joinable())
        {
            s.writerThread.join();
        }
    }

    void IpcServer::SendEvent(std::string_view name)
    {
        auto& s = *m_state;
        std::shared_ptr<PipeConnection> target;
        {
            std::lock_guard lock(s.mutex);
            target = s.connection;
        }
        Message event;
        event.type = MsgType::Event;
        event.eventName = std::string(name);
        s.Enqueue(std::move(target), std::move(event));
    }
}
