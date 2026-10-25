#pragma once

#include <optional>

#include <winrt/base.h>

#include "Previewers/PreviewContext.h"

namespace fastlook::viewer
{
    // A PreviewError thrown inside a PrepareAsync coroutine travels through IAsyncAction as an HRESULT error, and C++/WinRT
    // cannot convert foreign exception types (undefined behaviour). The bridge teaches it to (::winrt_to_hresult_handler),
    // and the session reads the error back with TryDecodePreviewError. Call InstallPreviewErrorBridge once at startup.
    void InstallPreviewErrorBridge();
    std::optional<PreviewError> TryDecodePreviewError(winrt::hresult_error const& error);
}
