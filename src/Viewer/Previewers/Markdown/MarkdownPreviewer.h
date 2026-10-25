#pragma once

#include <filesystem>
#include <memory>
#include <stop_token>
#include <thread>

#include "Markdown/MdModel.h"
#include "Previewers/IPreviewer.h"
#include "Previewers/Markdown/MdRenderer.h"
#include "Previewers/Text/CodeView.h"

namespace fastlook::viewer
{
    // Rendered markdown with a toolbar toggle to the highlighted source (a CodeView). Documents too large to render as XAML open
    // as source only.
    class MarkdownPreviewer final : public IPreviewer, public IToolbarProvider, public IKeyTarget, public ICommandTarget, public IZoomable
    {
    public:
        ~MarkdownPreviewer() override;

        winrt::Windows::Foundation::IAsyncAction PrepareAsync(PreviewItem item, std::shared_ptr<PreviewContext const> ctx) override;
        winrt::Microsoft::UI::Xaml::UIElement View() override;
        winrt::Windows::Foundation::Size PreferredSize() const override;
        void Unload() override;

        std::vector<winrt::Microsoft::UI::Xaml::UIElement> ToolbarItems() override;
        bool OnKey(winrt::Windows::System::VirtualKey key, bool ctrl, bool shift) override;
        bool Execute(std::wstring_view command) override;
        void Zoom(ZoomCommand command) override;

    private:
        void StopIndexing();
        void ShowMode(bool source);
        void ToggleMode();
        bool ScrollRendered(winrt::Windows::System::VirtualKey key);
        bool CopySelection();

        std::shared_ptr<PreviewContext const> m_ctx;
        std::shared_ptr<core::TextDocument> m_doc;
        std::shared_ptr<ILineStyler> m_styler;
        core::md::Document m_markdown;
        std::filesystem::path m_baseDir;
        bool m_sourceOnly = false;
        bool m_source = false;

        winrt::Microsoft::UI::Xaml::Controls::Grid m_root{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::ScrollViewer m_rendered{nullptr};
        std::unique_ptr<CodeView> m_code;
        std::shared_ptr<MdImages> m_images;
        std::stop_source m_renderStop;
        std::jthread m_indexThread; // prefetch of the line index while the rendered view is shown
        winrt::Microsoft::UI::Xaml::Controls::Button m_toggle{nullptr};
    };
}
