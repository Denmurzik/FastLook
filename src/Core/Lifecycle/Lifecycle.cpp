#include "Lifecycle/Lifecycle.h"

namespace fastlook::core
{
    Lifecycle::Lifecycle(uint64_t idleMs, uint64_t nowMs) : m_idleMs(idleMs), m_idleSince(nowMs) {}

    void Lifecycle::Idle(uint64_t nowMs)
    {
        m_idleStarted = true;
        m_idleSince = nowMs;
        m_trimDue = nowMs + kTrimDelayMs;
    }

    LifecycleAction Lifecycle::On(ViewerEvent event, uint64_t nowMs)
    {
        LifecycleAction action{};
        switch (event)
        {
        case ViewerEvent::PrewarmDone:
            if (Quiet())
            {
                Idle(nowMs);
            }
            break;
        case ViewerEvent::PreviewShown:
            m_visible = true;
            m_trimDue = 0;
            break;
        case ViewerEvent::PreviewHidden:
            m_visible = false;
            action.unloadContent = true;
            if (Quiet())
            {
                Idle(nowMs);
            }
            break;
        case ViewerEvent::PinnedOpened:
            ++m_pinned;
            m_trimDue = 0;
            break;
        case ViewerEvent::PinnedClosed:
            if (m_pinned)
            {
                --m_pinned;
            }
            if (Quiet())
            {
                Idle(nowMs);
            }
            break;
        case ViewerEvent::SettingsOpened:
            m_settings = true;
            m_trimDue = 0;
            break;
        case ViewerEvent::SettingsClosed:
            m_settings = false;
            if (Quiet())
            {
                Idle(nowMs);
            }
            break;
        case ViewerEvent::Tick:
            break;
        }
        if (Quiet() && m_trimDue && nowMs >= m_trimDue)
        {
            action.trimNow = true;
            m_trimDue = 0;
        }
        // Before the first PrewarmDone (a cold start can take longer than the idle time) only an explicit exit request counts.
        if (Quiet() && (m_exitWhenQuiet || (m_idleStarted && m_idleMs && nowMs - m_idleSince >= m_idleMs)))
        {
            action.exitNow = true;
        }
        return action;
    }
}
