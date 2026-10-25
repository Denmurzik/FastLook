#include "SharedState.h"

namespace fastlook::daemon
{
    std::atomic<DWORD> g_viewerPid{0};
    std::atomic<uint64_t> g_lastTriggerQpc{0};
}
