#pragma once

#include <functional>
#include <optional>
#include <string>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.Foundation.h>

#include "FileTypes/Classify.h"
#include "Ipc/Protocol.h"
#include "Settings/Settings.h"

namespace fastlook::viewer
{
    struct PreviewItem
    {
        core::ipc::Item ref;
        core::PreviewerKind kind = core::PreviewerKind::Fallback;
        std::optional<std::wstring> shellHandlerClsid;
        bool cloudPlaceholder = false; // decision 56: never read, never hydrated
    };

    struct FallbackInfo
    {
        std::wstring message;
        std::optional<std::wstring> storeProductId;
    };

    // Thrown from PrepareAsync: the session shows the Fallback previewer with this message (and Store link).
    struct PreviewError
    {
        std::wstring message;
        std::optional<std::wstring> storeProductId;
    };

    // Shared, immutable after PrepareAsync starts.
    struct PreviewContext
    {
        HWND hwnd = nullptr;
        winrt::Microsoft::UI::Dispatching::DispatcherQueue ui{nullptr};
        winrt::Windows::Foundation::Size maxContentDip{};
        double scale = 1.0;
        bool dark = false;
        core::Settings settings;
        std::optional<FallbackInfo> fallback;
        std::function<void()> requestResize; // thread-safe: asks the session to refit the window to PreferredSize()
    };
}
