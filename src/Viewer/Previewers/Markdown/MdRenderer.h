#pragma once

#include <filesystem>
#include <memory>
#include <stop_token>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

#include "Markdown/MdModel.h"

namespace fastlook::viewer
{
    // Per-document state of a rendered view: the decoded pictures and the text block that holds the current selection. UI thread
    // only. Close() releases them (and stops pending loads from attaching more); the owner calls it from Unload.
    struct MdImages
    {
        std::vector<winrt::Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource> sources;
        winrt::Microsoft::UI::Xaml::Controls::RichTextBlock selected{nullptr};
        bool closed = false;

        // The selected text of the block that has a selection, empty when nothing is selected.
        winrt::hstring SelectedText() const;
        void Close();
    };

    // UI thread. ScrollViewer > StackPanel of blocks. Local images (resolved against `baseDir`) are decoded one after the other in
    // the background and attached when ready; remote images (http, https, data, //, UNC) are never fetched and show their alt
    // text. Links open only for http, https and mailto. A requested stop cancels the pending image loads. When `images` is given
    // it receives the holder of the decoded pictures (close it in Unload).
    winrt::Microsoft::UI::Xaml::UIElement RenderMarkdown(core::md::Document const& document, std::filesystem::path const& baseDir,
                                                         bool dark, std::stop_token stop, std::shared_ptr<MdImages>* images = nullptr);
}
