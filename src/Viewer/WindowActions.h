#pragma once

#include "Previewers/PreviewContext.h"

namespace fastlook::viewer::actions
{
    // Stubs until the window actions task: they log and do nothing.
    void OpenDefault(PreviewItem const& item);
    void OpenWith(HWND owner, PreviewItem const& item);
    void Reveal(PreviewItem const& item);
    bool CopyFile(HWND owner, PreviewItem const& item);
}
