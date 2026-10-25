#include "PreviewController.h"

#include <filesystem>

#include "Diagnostics/Log.h"

namespace fastlook::daemon
{
    void PreviewController::Start(HWND, core::Settings const&, bool warm)
    {
        FL_LOG_INFO(L"previewController start warm={}", warm);
    }

    void PreviewController::Stop()
    {
        FL_LOG_INFO(L"previewController stop");
    }

    void PreviewController::OnTrigger(HWND)
    {
        FL_LOG_INFO(L"trigger");
    }

    void PreviewController::ShowPath(std::wstring const& path)
    {
        FL_LOG_INFO(L"showPath {}", core::log::RedactPath(path));
    }

    void PreviewController::TogglePath(std::wstring const& path)
    {
        FL_LOG_INFO(L"togglePath {}", core::log::RedactPath(path));
    }

    void PreviewController::OpenSettings(std::wstring_view page)
    {
        FL_LOG_INFO(L"openSettings page={}", page);
    }

    void PreviewController::OnSettingsChanged(core::Settings const&)
    {
        FL_LOG_INFO(L"settingsChanged");
    }

    void PreviewController::Shutdown()
    {
        FL_LOG_INFO(L"previewController shutdown");
    }
}
