#pragma once

#include <coroutine>

#include <winrt/Microsoft.UI.Dispatching.h>

namespace fastlook::viewer
{
    // `co_await ResumeOn(queue)` continues the coroutine on the queue's thread (no suspension when already there) and yields
    // true. When the queue is shutting down it resumes in place (so the frame is not leaked) and yields false: the caller
    // must co_return at once and not touch UI state, because it is not on the UI thread.
    struct ResumeOn
    {
        winrt::Microsoft::UI::Dispatching::DispatcherQueue queue;
        mutable bool resumedOnQueue = true;

        explicit ResumeOn(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& q) : queue(q) {}

        bool await_ready() const { return queue.HasThreadAccess(); }
        bool await_suspend(std::coroutine_handle<> handle) const
        {
            if (!queue.TryEnqueue([handle] { handle(); }))
            {
                resumedOnQueue = false;
                return false; // resume in place
            }
            return true;
        }
        [[nodiscard]] bool await_resume() const noexcept { return resumedOnQueue; }
    };
}
