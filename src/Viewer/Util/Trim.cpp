#include "pch.h"
#include "Util/Trim.h"

#include "Diagnostics/PerfLog.h"

namespace fastlook::viewer
{
    void TrimWorkingSet()
    {
        HeapCompact(GetProcessHeap(), 0);
        SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
        core::perf::Mark("trim");
    }
}
