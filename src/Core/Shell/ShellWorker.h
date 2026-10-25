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

        template <class R, class F>
        struct Job
        {
            explicit Job(F&& f) : fn(std::move(f)) {}

            void Execute()
            {
                std::optional<R> value;
                try
                {
                    value.emplace(std::invoke(fn));
                }
                catch (...)
                {
                }
                std::lock_guard lock(mutex);
                result = std::move(value);
                done = true;
                cv.notify_all();
            }

            F fn;
            std::mutex mutex;
            std::condition_variable cv;
            bool done = false;
            std::optional<R> result;
        };

        // Current worker thread, spawned if needed; nullptr when the abandoned cap is reached, the thread cannot
        // be started, or it is not ready by the deadline (then it is abandoned).
        std::shared_ptr<Thread> EnsureThread(std::chrono::steady_clock::time_point deadline);
        static bool Post(Thread& thread, std::function<void()> work);
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
        using Callable = std::decay_t<F>;
        static_assert(!std::is_void_v<R>, "ShellWorker::Run needs a result type");

        auto deadline = std::chrono::steady_clock::now() + timeout;
        auto job = std::make_shared<Job<R, Callable>>(Callable(std::forward<F>(fn)));
        auto thread = EnsureThread(deadline); // also enforces the abandoned cap: hung Explorer fails fast
        if (!thread || !Post(*thread, [job] { job->Execute(); }))
            return std::nullopt;

        std::unique_lock lock(job->mutex);
        if (!job->cv.wait_until(lock, deadline, [&] { return job->done; }))
        {
            // Completion and abandonment are decided under the job lock: a job that just finished keeps its thread.
            Retire(thread);
            return std::nullopt;
        }
        return std::move(job->result);
    }
}
