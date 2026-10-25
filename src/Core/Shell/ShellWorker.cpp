#include "Shell/ShellWorker.h"

#include "Shell/ExplorerQuery.h"

#include <Windows.h>
#include <objbase.h>

#include <deque>
#include <thread>

#pragma comment(lib, "ole32.lib")

namespace fastlook::core
{
    namespace
    {
        constexpr UINT kWakeMessage = WM_APP + 1;
    }

    // Shared between the owning ShellWorker and the OS thread, so an abandoned thread outlives the worker safely.
    struct ShellWorker::Thread
    {
        std::mutex mutex;
        std::condition_variable readyCv;
        bool ready = false;
        bool initFailed = false; // COM could not be initialised: the thread exits at once
        bool poisoned = false;   // takes no new jobs; also set when the thread dies by itself
        bool abandoned = false; // poisoned because of a timeout: counted in abandonedCount until the thread exits
        DWORD threadId = 0;
        std::deque<std::function<void()>> queue;
        std::shared_ptr<std::atomic<uint32_t>> abandonedCount;

        // Next queued job, or false when the thread must exit (poisoned) or there is nothing to do yet.
        bool Next(std::function<void()>& work, bool& exit)
        {
            std::lock_guard lock(mutex);
            if (poisoned)
            {
                queue.clear();
                exit = true;
                return false;
            }
            if (queue.empty())
                return false;
            work = std::move(queue.front());
            queue.pop_front();
            return true;
        }

        void Main()
        {
            bool comOk = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
            MSG msg;
            PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // creates the message queue
            {
                std::lock_guard lock(mutex);
                threadId = GetCurrentThreadId();
                initFailed = !comOk;
                ready = true;
            }
            readyCv.notify_all();

            bool exit = !comOk;
            while (!exit)
            {
                std::function<void()> work;
                while (Next(work, exit))
                {
                    work();
                    work = nullptr;
                }
                if (exit || GetMessageW(&msg, nullptr, 0, 0) <= 0)
                    break;
                if (msg.message != kWakeMessage)
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            if (comOk)
            {
                ReleaseExplorerThreadState(); // COM objects must be released before the apartment goes away
                CoUninitialize();
            }
            std::lock_guard lock(mutex);
            poisoned = true; // WM_QUIT or a failed GetMessage ends the thread without a Poison call
            queue.clear();
            if (abandoned)
                abandonedCount->fetch_sub(1);
        }
    };

    ShellWorker::ShellWorker() : m_abandoned(std::make_shared<std::atomic<uint32_t>>(0)) {}

    ShellWorker::~ShellWorker()
    {
        std::shared_ptr<Thread> thread;
        {
            std::lock_guard lock(m_mutex);
            thread = std::move(m_thread);
        }
        if (thread)
            Poison(*thread, false); // exits once idle; never blocks the destructor
    }

    uint32_t ShellWorker::AbandonedCount() const
    {
        return m_abandoned->load();
    }

    std::shared_ptr<ShellWorker::Thread> ShellWorker::EnsureThread(std::chrono::steady_clock::time_point deadline)
    {
        std::lock_guard lock(m_mutex);
        if (m_abandoned->load() >= kMaxAbandoned)
            return nullptr;
        if (m_thread)
        {
            bool alive;
            {
                std::lock_guard tlock(m_thread->mutex);
                alive = !m_thread->poisoned;
            }
            if (alive)
                return m_thread;
            m_thread.reset();
        }
        auto thread = std::make_shared<Thread>();
        thread->abandonedCount = m_abandoned;
        try
        {
            std::thread([thread] { thread->Main(); }).detach();
        }
        catch (...)
        {
            return nullptr;
        }
        std::unique_lock tlock(thread->mutex);
        if (!thread->readyCv.wait_until(tlock, deadline, [&] { return thread->ready; }))
        {
            tlock.unlock();
            Poison(*thread, true); // stuck while starting: counts as abandoned, exits when it gets going
            return nullptr;
        }
        if (thread->initFailed)
            return nullptr;
        m_thread = thread;
        return thread;
    }

    bool ShellWorker::Post(Thread& thread, std::function<void()> work)
    {
        DWORD id;
        {
            std::lock_guard lock(thread.mutex);
            if (thread.poisoned)
                return false;
            thread.queue.push_back(std::move(work));
            id = thread.threadId;
        }
        PostThreadMessageW(id, kWakeMessage, 0, 0);
        return true;
    }

    void ShellWorker::Poison(Thread& thread, bool abandoned)
    {
        DWORD id;
        {
            std::lock_guard lock(thread.mutex);
            if (thread.poisoned)
                return;
            thread.poisoned = true;
            thread.abandoned = abandoned;
            if (abandoned)
                thread.abandonedCount->fetch_add(1);
            id = thread.threadId;
        }
        PostThreadMessageW(id, kWakeMessage, 0, 0);
    }

    void ShellWorker::Retire(std::shared_ptr<Thread> const& thread)
    {
        Poison(*thread, true);
        std::lock_guard lock(m_mutex);
        if (m_thread == thread)
            m_thread.reset();
    }
}
