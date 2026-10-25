#pragma once

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "Ipc/HostProtocol.h"

namespace fastlook::viewer
{
    // One FastLook.PreviewHost.exe child, owned by a Viewer-wide job (KILL_ON_JOB_CLOSE: the host dies with the Viewer, even
    // when the Viewer is killed; the daemon's own job does not reach it because that one allows silent breakaway).
    //
    // Nothing here blocks the calling thread on the host, except WaitReady (background threads only) and WaitExit (bounded by
    // the caller). Commands go through a writer thread so a hung host (full stdin pipe) cannot stall the UI thread.
    class HostProcess
    {
    public:
        using KeyHandler = std::function<void(uint32_t vk, uint32_t mods)>; // called on a reader thread

        enum class Outcome
        {
            Ready,
            Error,     // the host reported `error`
            Exited,    // the process ended without `ready`
            Timeout,
            Cancelled
        };

        struct Result
        {
            Outcome outcome = Outcome::Exited;
            uint32_t hr = 0;
            std::string text;
        };

        // Null (with `error` set) when the process could not be started.
        static std::shared_ptr<HostProcess> Launch(std::filesystem::path const& exe, core::host::HostArgs const& args, KeyHandler onKey,
                                                   std::wstring& error);
        ~HostProcess();

        // Waits for `ready` / `error` / process exit. Polls `cancelled` every ~50 ms. Never call on the UI thread.
        Result WaitReady(std::chrono::milliseconds timeout, std::function<bool()> const& cancelled);

        void SendRect(core::RectI const& rect); // non-blocking; only the latest pending rect is kept
        void RequestQuit();                     // non-blocking
        // True when the process has ended (waits at most `ms`).
        bool WaitExit(DWORD ms) const;
        // Our own host only; idempotent.
        void Terminate();
        // After this no more key callbacks are delivered.
        void DetachKeyHandler();

    private:
        struct State;
        explicit HostProcess(std::shared_ptr<State> state) : m_state(std::move(state)) {}
        std::shared_ptr<State> m_state;
    };
}
