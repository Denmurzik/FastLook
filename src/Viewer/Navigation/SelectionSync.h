#pragma once

#include <Windows.h>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

#include "Ipc/Protocol.h"

namespace fastlook::viewer
{
    // Keeps the Explorer selection on the item the Viewer shows (folder navigation only). Sync() returns at once; one background
    // thread (started by the first Sync) owns a ShellWorker and runs only the latest pending request (latest wins) with the
    // 500 ms selection-sync timeout. The result is ignored.
    class SelectionSync
    {
    public:
        SelectionSync();
        ~SelectionSync();
        SelectionSync(SelectionSync const&) = delete;
        SelectionSync& operator=(SelectionSync const&) = delete;

        void Sync(HWND explorer, bool isDesktop, core::ipc::Item item);

    private:
        struct Request
        {
            HWND explorer = nullptr;
            bool isDesktop = false;
            core::ipc::Item item;
        };

        void Loop(std::stop_token stop);

        std::mutex m_mutex;
        std::condition_variable_any m_wake;
        std::optional<Request> m_pending;
        std::jthread m_thread; // last: joined first; started lazily under m_mutex
    };
}
