#include "pch.h"
#include "Previewers/Shell/HostProcess.h"

#include <condition_variable>
#include <mutex>
#include <thread>

#include "Diagnostics/Log.h"

namespace fastlook::viewer
{
    namespace proto = core::host;

    namespace
    {
        constexpr auto kPollSlice = std::chrono::milliseconds(50);

        // Owned by this Viewer process for its whole life; closed by the OS when the process ends, which kills every host in it.
        HANDLE ViewerJob()
        {
            static HANDLE job = [] {
                HANDLE h = CreateJobObjectW(nullptr, nullptr);
                if (!h)
                {
                    return HANDLE{};
                }
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
                info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
                if (!SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof(info)))
                {
                    CloseHandle(h);
                    return HANDLE{};
                }
                return h;
            }();
            return job;
        }

        struct ScopedHandle
        {
            HANDLE h = nullptr;
            ScopedHandle() = default;
            explicit ScopedHandle(HANDLE handle) : h(handle) {}
            ScopedHandle(ScopedHandle const&) = delete;
            ScopedHandle& operator=(ScopedHandle const&) = delete;
            ~ScopedHandle() { reset(); }
            void reset(HANDLE next = nullptr)
            {
                if (h && h != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(h);
                }
                h = next;
            }
            HANDLE release() { return std::exchange(h, nullptr); }
            explicit operator bool() const { return h && h != INVALID_HANDLE_VALUE; }
        };

        std::wstring ErrorText(wchar_t const* what)
        {
            return std::wstring(what) + L" (" + std::to_wstring(GetLastError()) + L")";
        }
    }

    struct HostProcess::State
    {
        ScopedHandle process, stdinWrite, stdoutRead;

        std::mutex mutex;
        std::condition_variable cv;
        // Reader results.
        bool ready = false;
        bool outputClosed = false;
        std::optional<proto::HostReply> error;
        KeyHandler onKey;
        // Writer queue.
        std::optional<core::RectI> pendingRect;
        bool quit = false;
        bool stop = false;
        bool terminated = false;
    };

    std::shared_ptr<HostProcess> HostProcess::Launch(std::filesystem::path const& exe, proto::HostArgs const& args, KeyHandler onKey,
                                                     std::wstring& error)
    {
        SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
        ScopedHandle childStdinRead, stdinWrite, stdoutRead, childStdoutWrite;
        HANDLE a = nullptr, b = nullptr;
        if (!CreatePipe(&a, &b, &inheritable, 0))
        {
            error = ErrorText(L"CreatePipe(stdin)");
            return nullptr;
        }
        childStdinRead.reset(a);
        stdinWrite.reset(b);
        if (!CreatePipe(&a, &b, &inheritable, 0))
        {
            error = ErrorText(L"CreatePipe(stdout)");
            return nullptr;
        }
        stdoutRead.reset(a);
        childStdoutWrite.reset(b);
        // Our ends stay private to this process.
        SetHandleInformation(stdinWrite.h, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stdoutRead.h, HANDLE_FLAG_INHERIT, 0);

        // Only the two child ends are inherited (other processes are launched concurrently by this Viewer).
        HANDLE inheritList[2] = {childStdinRead.h, childStdoutWrite.h};
        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        std::unique_ptr<std::byte[]> attributeStorage(new std::byte[attributeBytes]);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.get());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes) ||
            !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritList, sizeof(inheritList), nullptr, nullptr))
        {
            error = ErrorText(L"attribute list");
            return nullptr;
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = childStdinRead.h;
        startup.StartupInfo.hStdOutput = childStdoutWrite.h;
        startup.StartupInfo.hStdError = INVALID_HANDLE_VALUE;
        startup.lpAttributeList = attributes;

        std::wstring commandLine = L"\"" + exe.wstring() + L"\" " + proto::FormatHostArgs(args);
        PROCESS_INFORMATION pi{};
        // Suspended, so the process is in the job before it runs its first instruction.
        BOOL const created = CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                                            CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                            &startup.StartupInfo, &pi);
        DWORD const createError = GetLastError();
        DeleteProcThreadAttributeList(attributes);
        if (!created)
        {
            SetLastError(createError);
            error = ErrorText(L"CreateProcess");
            return nullptr;
        }
        ScopedHandle thread(pi.hThread);
        auto state = std::make_shared<State>();
        state->process.reset(pi.hProcess);

        HANDLE const job = ViewerJob();
        if (!job || !AssignProcessToJobObject(job, pi.hProcess))
        {
            // Without the job a dying Viewer would leave the host behind: do not run it.
            error = ErrorText(L"AssignProcessToJobObject");
            TerminateProcess(pi.hProcess, 1);
            return nullptr;
        }
        ResumeThread(pi.hThread);

        childStdinRead.reset();
        childStdoutWrite.reset();
        state->stdinWrite.reset(stdinWrite.release());
        state->stdoutRead.reset(stdoutRead.release());
        state->onKey = std::move(onKey);

        // Reader: parses the host's lines.
        std::thread([state] {
            std::string pending;
            char buffer[512];
            for (;;)
            {
                DWORD read = 0;
                if (!ReadFile(state->stdoutRead.h, buffer, sizeof(buffer), &read, nullptr) || read == 0)
                {
                    break;
                }
                pending.append(buffer, read);
                size_t newline;
                while ((newline = pending.find('\n')) != std::string::npos)
                {
                    auto reply = proto::ParseHostReply(std::string_view(pending).substr(0, newline));
                    pending.erase(0, newline + 1);
                    if (!reply)
                    {
                        continue;
                    }
                    KeyHandler keyHandler;
                    {
                        std::lock_guard lock{state->mutex};
                        if (reply->status == proto::HostStatus::Ready)
                        {
                            state->ready = true;
                        }
                        else if (reply->status == proto::HostStatus::Error)
                        {
                            state->error = *reply;
                        }
                        else
                        {
                            keyHandler = state->onKey;
                        }
                    }
                    state->cv.notify_all();
                    if (keyHandler)
                    {
                        keyHandler(reply->vk, reply->mods);
                    }
                }
            }
            {
                std::lock_guard lock{state->mutex};
                state->outputClosed = true;
            }
            state->cv.notify_all();
        }).detach();

        // Writer: the UI thread only queues.
        std::thread([state] {
            for (;;)
            {
                std::string data;
                bool sendsQuit = false;
                {
                    std::unique_lock lock{state->mutex};
                    state->cv.wait(lock, [&] { return state->stop || state->quit || state->pendingRect.has_value(); });
                    if (state->stop)
                    {
                        return;
                    }
                    if (state->pendingRect)
                    {
                        data = proto::FormatHostCommand({proto::HostCmd::Rect, *state->pendingRect});
                        state->pendingRect.reset();
                    }
                    if (state->quit)
                    {
                        data += proto::FormatHostCommand({proto::HostCmd::Quit, {}});
                        sendsQuit = true;
                    }
                }
                DWORD written = 0;
                if (!WriteFile(state->stdinWrite.h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr))
                {
                    return; // the host is gone
                }
                if (sendsQuit)
                {
                    return;
                }
            }
        }).detach();

        return std::shared_ptr<HostProcess>(new HostProcess(std::move(state)));
    }

    HostProcess::~HostProcess()
    {
        if (!m_state)
        {
            return;
        }
        Terminate();
        {
            std::lock_guard lock{m_state->mutex};
            m_state->stop = true;
            m_state->onKey = nullptr;
        }
        m_state->cv.notify_all();
    }

    HostProcess::Result HostProcess::WaitReady(std::chrono::milliseconds timeout, std::function<bool()> const& cancelled)
    {
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        for (;;)
        {
            {
                std::unique_lock lock{m_state->mutex};
                m_state->cv.wait_for(lock, kPollSlice, [&] { return m_state->ready || m_state->error || m_state->outputClosed; });
                if (m_state->ready)
                {
                    return {Outcome::Ready};
                }
                if (m_state->error)
                {
                    return {Outcome::Error, m_state->error->hr, m_state->error->text};
                }
                if (m_state->outputClosed)
                {
                    return {Outcome::Exited};
                }
            }
            if (WaitForSingleObject(m_state->process.h, 0) == WAIT_OBJECT_0)
            {
                return {Outcome::Exited};
            }
            if (cancelled && cancelled())
            {
                return {Outcome::Cancelled};
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return {Outcome::Timeout};
            }
        }
    }

    void HostProcess::SendRect(core::RectI const& rect)
    {
        {
            std::lock_guard lock{m_state->mutex};
            m_state->pendingRect = rect;
        }
        m_state->cv.notify_all();
    }

    void HostProcess::RequestQuit()
    {
        {
            std::lock_guard lock{m_state->mutex};
            m_state->quit = true;
        }
        m_state->cv.notify_all();
    }

    bool HostProcess::WaitExit(DWORD ms) const
    {
        return WaitForSingleObject(m_state->process.h, ms) == WAIT_OBJECT_0;
    }

    void HostProcess::Terminate()
    {
        {
            std::lock_guard lock{m_state->mutex};
            if (m_state->terminated)
            {
                return;
            }
            m_state->terminated = true;
        }
        if (WaitForSingleObject(m_state->process.h, 0) != WAIT_OBJECT_0)
        {
            TerminateProcess(m_state->process.h, 1);
        }
    }

    void HostProcess::DetachKeyHandler()
    {
        std::lock_guard lock{m_state->mutex};
        m_state->onKey = nullptr;
    }
}
