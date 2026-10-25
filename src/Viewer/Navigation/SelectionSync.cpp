#include "pch.h"
#include "Navigation/SelectionSync.h"

#include <chrono>

#include "Shell/ExplorerQuery.h"
#include "Shell/ShellWorker.h"

namespace fastlook::viewer
{
    namespace
    {
        constexpr std::chrono::milliseconds kSelectionSyncTimeout{500};
    }

    SelectionSync::SelectionSync() : m_thread([this](std::stop_token stop) { Loop(stop); }) {}

    SelectionSync::~SelectionSync()
    {
        m_thread.request_stop();
        m_wake.notify_all();
    }

    void SelectionSync::Sync(HWND explorer, bool isDesktop, core::ipc::Item item)
    {
        {
            std::lock_guard lock{m_mutex};
            m_pending = Request{explorer, isDesktop, std::move(item)};
        }
        m_wake.notify_one();
    }

    void SelectionSync::Loop(std::stop_token stop)
    {
        core::ShellWorker worker;
        for (;;)
        {
            Request request;
            {
                std::unique_lock lock{m_mutex};
                if (!m_wake.wait(lock, stop, [&] { return m_pending.has_value(); }))
                {
                    return; // stop requested
                }
                request = std::move(*m_pending);
                m_pending.reset();
            }
            // The job owns copies of everything: an abandoned (timed-out) call may still run after Run returned.
            worker.Run(
                [explorer = request.explorer, isDesktop = request.isDesktop, item = std::move(request.item)] {
                    return core::SelectInExplorer(explorer, isDesktop, item);
                },
                kSelectionSyncTimeout);
        }
    }
}
