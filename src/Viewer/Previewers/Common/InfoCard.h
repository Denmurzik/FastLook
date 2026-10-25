#pragma once

#include <functional>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include "Previewers/Common/ShellInfo.h"

namespace fastlook::viewer
{
    struct InfoCardRow
    {
        winrt::hstring label;
        winrt::hstring value;
    };

    struct InfoCardButton
    {
        winrt::hstring label;
        std::function<void()> onClick;
        bool hyperlink = false; // a link-style button instead of a standard one
    };

    struct InfoCardModel
    {
        winrt::Microsoft::UI::Xaml::Media::ImageSource image{nullptr};
        double imageSizeDip = 256;
        winrt::hstring title;
        winrt::hstring message; // shown between the title and the rows when not empty
        std::vector<InfoCardRow> rows;
        std::vector<InfoCardButton> buttons;
    };

    // Type, size (when withSize), created, modified and attributes rows from the shell data; empty values are skipped.
    void AddShellRows(InfoCardModel& model, ShellInfo const& info, bool withSizeAndAttributes);

    // UI thread. When `valueBlocks` is given it receives, per row, the TextBlock showing the value (for live updates).
    winrt::Microsoft::UI::Xaml::UIElement BuildInfoCard(InfoCardModel const& model,
                                                         std::vector<winrt::Microsoft::UI::Xaml::Controls::TextBlock>* valueBlocks = nullptr);
}
