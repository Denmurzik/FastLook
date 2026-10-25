#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace fastlook::core
{
    // Runs shell/COM work on a single-threaded-apartment thread with a hard caller-side timeout.
    // A call that outlives its timeout poisons the thread: it is abandoned (finishes the stuck call, takes no
    // new jobs, uninitialises COM, exits) and the next Run spawns a fresh one. The caller never blocks past
    // the timeout. An abandoned job may still run after Run returned, so fn must own everything it touches.
    class ShellWorker
    {
    public:
        ShellWorker();
        ~ShellWorker();
        ShellWorker(ShellWorker const&) = delete;
        ShellWorker& operator=(ShellWorker const&) = delete;

        // R must be non-void. Returns nullopt on timeout, when fn throws, or when kMaxAbandoned threads are stuck.
        template <class F>
        auto Run(F&& fn, std::chrono::milliseconds timeout) -> std::optional<std::invoke_result_t<F>>;

        // Abandoned threads still alive (each decrements when its stuck call returns).
        uint32_t AbandonedCount() const;
        static constexpr uint32_t kMaxAbandoned = 3;

    private:
        struct Thread;

        template <class R>
        struct Job
        {
            explicit Job(std::function<R()> f) : fn(std::move(f)) {}

            void Execute()
            {
                std::optional<R> value;
                try
                {
                    value.emplace(fn());
                }
                catch (...)
                {
                }
                std::lock_guard lock(mutex);
                result = std::move(value);
                done = true;
                cv.notify_all();
            }

            bool WaitFor(std::chrono::milliseconds timeout)
            {
                std::unique_lock lock(mutex);
                return cv.wait_for(lock, timeout, [this] { return done; });
            }

            std::function<R()> fn;
            std::mutex mutex;
            std::condition_variable cv;
            bool done = false;
            std::optional<R> result;
        };

        std::shared_ptr<Thread> EnsureThread();
        static void Post(Thread& thread, std::function<void()> work);
        static void Poison(Thread& thread, bool abandoned);
        void Retire(std::shared_ptr<Thread> const& thread);

        std::shared_ptr<std::atomic<uint32_t>> m_abandoned;
        std::mutex m_mutex;
        std::shared_ptr<Thread> m_thread;
    };

    template <class F>
    auto ShellWorker::Run(F&& fn, std::chrono::milliseconds timeout) -> std::optional<std::invoke_result_t<F>>
    {
        using R = std::invoke_result_t<F>;
        static_assert(!std::is_void_v<R>, "ShellWorker::Run needs a result type");

        if (AbandonedCount() >= kMaxAbandoned)
            return std::nullopt; // hung Explorer: fail fast, no unbounded thread growth
        auto job = std::make_shared<Job<R>>(std::function<R()>(std::forward<F>(fn)));
        auto thread = EnsureThread();
        if (!thread)
            return std::nullopt;
        Post(*thread, [job] { job->Execute(); });
        if (job->WaitFor(timeout))
        {
            std::lock_guard lock(job->mutex);
            return std::move(job->result);
        }
        Retire(thread);
        return std::nullopt;
    }
}
