#pragma once

namespace fastlook::viewer
{
    // Compacts the process heap and drops the working set so the idle Viewer's Private Working Set falls back to the warm
    // baseline. GPU/D2D/DWrite/WIC objects are not touched (decision 55).
    void TrimWorkingSet();
}
