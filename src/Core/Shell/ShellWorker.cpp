#include "Shell/ShellWorker.h"

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
        bool poisoned = false;
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
                ready = true;
            }
            readyCv.notify_all();

            bool exit = false;
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
                CoUninitialize();
            std::lock_guard lock(mutex);
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

    std::shared_ptr<ShellWorker::Thread> ShellWorker::EnsureThread()
    {
        std::lock_guard lock(m_mutex);
        if (m_thread)
            return m_thread;
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
        thread->readyCv.wait(tlock, [&] { return thread->ready; });
        m_thread = thread;
        return thread;
    }

    void ShellWorker::Post(Thread& thread, std::function<void()> work)
    {
        DWORD id;
        {
            std::lock_guard lock(thread.mutex);
            thread.queue.push_back(std::move(work));
            id = thread.threadId;
        }
        PostThreadMessageW(id, kWakeMessage, 0, 0);
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
