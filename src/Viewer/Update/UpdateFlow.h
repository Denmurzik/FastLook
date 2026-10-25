#pragma once

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>

namespace fastlook::viewer
{
    // The "Check for updates" button of the About page: checks, asks and installs, reporting through dialogs on `root`.
    // Completes when the user is done. Never throws.
    winrt::Windows::Foundation::IAsyncAction CheckForUpdatesInteractive(winrt::Microsoft::UI::Xaml::XamlRoot root);
}
