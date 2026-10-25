#include "pch.h"
#include "Update/UpdateFlow.h"

#include "Diagnostics/Log.h"
#include "Util/Str.h"

namespace fastlook::viewer
{
    // Stub: replaced by the update task.
    winrt::Windows::Foundation::IAsyncAction CheckForUpdatesInteractive(winrt::Microsoft::UI::Xaml::XamlRoot root)
    {
        try
        {
            winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog;
            dialog.XamlRoot(root);
            dialog.Content(winrt::box_value(Str(L"Settings", L"UpdateNotImplemented")));
            dialog.CloseButtonText(Str(L"Settings", L"UpdateDialogClose"));
            co_await dialog.ShowAsync();
        }
        catch (winrt::hresult_error const& e)
        {
            FL_LOG_INFO(L"Update dialog failed: 0x{:08X}", static_cast<uint32_t>(e.code().value));
        }
    }
}
