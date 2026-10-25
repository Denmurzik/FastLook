#pragma once

#include <coroutine>

#include <winrt/Microsoft.UI.Dispatching.h>

namespace fastlook::viewer
{
    // `co_await ResumeOn(queue)` continues the coroutine on the queue's thread (no suspension when already there).
    // When the queue is shutting down the coroutine resumes in place rather than leaking its frame.
    struct ResumeOn
    {
        winrt::Microsoft::UI::Dispatching::DispatcherQueue queue;

        explicit ResumeOn(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& q) : queue(q) {}

        bool await_ready() const { return queue.HasThreadAccess(); }
        void await_suspend(std::coroutine_handle<> handle) const
        {
            if (!queue.TryEnqueue([handle] { handle(); }))
            {
                handle();
            }
        }
        void await_resume() const noexcept {}
    };
}
