#include "pch.h"
#include "WindowActions.h"

#include "Diagnostics/Log.h"

namespace fastlook::viewer::actions
{
    void OpenDefault(PreviewItem const&)
    {
        FL_LOG_INFO(L"actions::OpenDefault: not implemented");
    }

    void OpenWith(HWND, PreviewItem const&)
    {
        FL_LOG_INFO(L"actions::OpenWith: not implemented");
    }

    void Reveal(PreviewItem const&)
    {
        FL_LOG_INFO(L"actions::Reveal: not implemented");
    }

    bool CopyFile(HWND, PreviewItem const&)
    {
        FL_LOG_INFO(L"actions::CopyFile: not implemented");
        return false;
    }
}
