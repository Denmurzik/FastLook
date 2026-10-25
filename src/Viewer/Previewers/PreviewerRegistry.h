#pragma once

#include <memory>
#include <optional>

#include "FileTypes/Classify.h"
#include "Previewers/IPreviewer.h"

namespace fastlook::viewer
{
    std::shared_ptr<IPreviewer> CreatePreviewer(core::PreviewerKind kind);

    // A PreviewError thrown inside a PrepareAsync coroutine travels through IAsyncAction as an HRESULT error, and C++/WinRT
    // cannot convert foreign exception types. The bridge teaches it to (winrt_to_hresult_handler), and the session reads the
    // error back with TryDecodePreviewError. Call InstallPreviewErrorBridge once at startup.
    void InstallPreviewErrorBridge();
    std::optional<PreviewError> TryDecodePreviewError(winrt::hresult_error const& error);
}
