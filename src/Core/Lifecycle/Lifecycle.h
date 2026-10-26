#pragma once

#include <cstdint>

namespace fastlook::core
{
    enum class ViewerEvent
    {
        PrewarmDone,
        PreviewShown,
        PreviewHidden,
        PinnedOpened,
        PinnedClosed,
        SettingsOpened,
        SettingsClosed,
        Tick
    };

    struct LifecycleAction
    {
        bool unloadContent = false, trimNow = false, exitNow = false;
    };

    inline constexpr uint64_t kTrimDelayMs = 1000;
    // An exit requested with SetExitWhenQuiet happens on a Tick once the Viewer has been quiet this long, never inside the close
    // that made it quiet: the closed previewer may still be releasing (the media player closes ~100 ms after its window hides).
    inline constexpr uint64_t kQuietExitDelayMs = 1000;

    // The server Viewer's quiet-time state machine: when to unload content, trim the working set and exit. Pure logic with
    // an injected clock (milliseconds, any monotonic origin). "Quiet" = no preview, no pinned window, no settings window.
    class Lifecycle
    {
    public:
        // Starts hidden and quiet but not idle: the first trim and the idle clock begin at PrewarmDone.
        Lifecycle(uint64_t idleMs /*0 = never*/, uint64_t nowMs);

        LifecycleAction On(ViewerEvent event, uint64_t nowMs);
        void SetIdleMs(uint64_t idleMs) { m_idleMs = idleMs; }
        // Exit once nothing has been open for kQuietExitDelayMs, on a Tick (a language change or the memory ceiling needs a restart).
        void SetExitWhenQuiet() { m_exitWhenQuiet = true; }

    private:
        bool Quiet() const { return !m_visible && m_pinned == 0 && !m_settings; }
        void Idle(uint64_t nowMs);

        uint64_t m_idleMs;
        uint64_t m_idleSince = 0;
        uint64_t m_trimDue = 0;
        bool m_idleStarted = false;
        bool m_visible = false;
        bool m_settings = false;
        bool m_exitWhenQuiet = false;
        int m_pinned = 0;
    };
}
