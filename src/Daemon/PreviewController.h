#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "Settings/Settings.h"

namespace fastlook::core
{
    class ShellWorker;
}

namespace fastlook::daemon
{
    class ViewerManager;
    class Prewarm;

    // Every public method is called on the main thread and only queues work: all shell-worker and pipe calls run on the
    // controller thread, so the main thread (and the CLI forwarding into WM_COPYDATA) never blocks.
    class PreviewController
    {
    public:
        PreviewController();
        ~PreviewController();
        PreviewController(PreviewController const&) = delete;
        PreviewController& operator=(PreviewController const&) = delete;

        void Start(HWND mainWnd, core::Settings const& settings, bool warm);
        void Stop();
        void OnTrigger(HWND foreground);
        void ShowPath(std::wstring const& path);
        void TogglePath(std::wstring const& path);
        void OpenSettings(std::wstring_view page = L"general");
        void OnSettingsChanged(core::Settings const& settings);
        void Shutdown();

    private:
        using Task = std::function<void()>;

        void Post(Task task);
        void ThreadMain();
        void RunTrigger(HWND foreground, uint64_t sequence);
        void SendSinglePath(std::wstring const& path, bool toggle);
        void OnForegroundCandidate(bool isDesktop);
        bool PrewarmAllowed(bool isDesktop);

        std::unique_ptr<core::ShellWorker> m_shell;
        std::unique_ptr<ViewerManager> m_viewer;
        std::unique_ptr<Prewarm> m_prewarm;
        HWND m_main = nullptr;

        std::mutex m_mutex; // guards m_tasks, m_stop, m_done, m_settings
        std::condition_variable m_cv;
        std::deque<Task> m_tasks;
        bool m_started = false;
        bool m_stop = false;
        bool m_done = false;
        core::Settings m_settings;
        std::thread m_thread;
        std::atomic<uint64_t> m_triggerSeq{0};
        ULONGLONG m_lastPrewarmTick = 0; // main thread only
        std::chrono::steady_clock::time_point m_lastJobEnd; // controller thread only
        bool m_trimmed = true;
    };
}
