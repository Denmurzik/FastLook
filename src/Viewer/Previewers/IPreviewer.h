#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>

#include "Previewers/PreviewContext.h"

namespace fastlook::viewer
{
    // Lifetime (decision 54): previewers are owned by shared_ptr. PrepareAsync takes its arguments by value and starts with
    // `auto self = shared_from_this();`, so a cancelled coroutine can never touch a destroyed previewer. The session calls
    // Unload() and drops its reference only after the (possibly cancelled) PrepareAsync has completed.
    // PrepareAsync may throw PreviewError; it must honour cancellation (co_await winrt::get_cancellation_token()).
    struct IPreviewer : std::enable_shared_from_this<IPreviewer>
    {
        virtual ~IPreviewer() = default;
        // Heavy work off the UI thread.
        virtual winrt::Windows::Foundation::IAsyncAction PrepareAsync(PreviewItem item, std::shared_ptr<PreviewContext const> ctx) = 0;
        // UI thread, after PrepareAsync completed successfully.
        virtual winrt::Microsoft::UI::Xaml::UIElement View() = 0;
        // DIP.
        virtual winrt::Windows::Foundation::Size PreferredSize() const = 0;
        virtual bool KeepAspect() const { return false; }
        // UI thread, only after PrepareAsync completed; releases the previewer's own resources.
        virtual void Unload() = 0;
    };

    enum class ZoomCommand
    {
        In,
        Out,
        Reset
    };

    struct IZoomable
    {
        virtual void Zoom(ZoomCommand) = 0;
    };

    struct IToolbarProvider
    {
        virtual std::vector<winrt::Microsoft::UI::Xaml::UIElement> ToolbarItems() = 0;
    };

    // Script command "cmd:<name>".
    struct ICommandTarget
    {
        virtual bool Execute(std::wstring_view command) = 0;
    };

    // E.g. Ctrl+A / Ctrl+C in CodeView.
    struct IKeyTarget
    {
        virtual bool OnKey(winrt::Windows::System::VirtualKey, bool ctrl, bool shift) = 0;
    };
}
