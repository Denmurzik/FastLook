#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "Previewers/IPreviewer.h"
#include "Previewers/Text/CodeView.h"

namespace fastlook::viewer
{
    // Plain text through a memory-mapped TextDocument: the line index is built on a background thread while the CodeView shows
    // what is indexed so far.
    class TextPreviewer final : public IPreviewer, public IToolbarProvider, public IKeyTarget, public ICommandTarget, public IZoomable
    {
    public:
        ~TextPreviewer() override;

        winrt::Windows::Foundation::IAsyncAction PrepareAsync(PreviewItem item, std::shared_ptr<PreviewContext const> ctx) override;
        winrt::Microsoft::UI::Xaml::UIElement View() override;
        winrt::Windows::Foundation::Size PreferredSize() const override;
        void Unload() override;

        std::vector<winrt::Microsoft::UI::Xaml::UIElement> ToolbarItems() override;
        bool OnKey(winrt::Windows::System::VirtualKey key, bool ctrl, bool shift) override;
        bool Execute(std::wstring_view command) override;
        void Zoom(ZoomCommand command) override;

    private:
        // Shared with the indexing thread, which may outlive a cancelled PrepareAsync by one chunk.
        struct IndexState
        {
            std::mutex mutex;
            std::condition_variable changed;
            bool firstReported = false; // under mutex: the first chunk was indexed, or indexing ended
            std::atomic<uint64_t> lines{0};
            std::atomic<bool> complete{false};
            std::atomic<bool> uiPending{false};
        };

        void StopIndexing();
        void DrainProgress(); // UI thread
        void ToggleWrap();
        bool CopySelection();

        std::shared_ptr<PreviewContext const> m_ctx;
        std::shared_ptr<core::TextDocument> m_doc;
        std::shared_ptr<IndexState> m_index;
        std::jthread m_indexThread;
        std::unique_ptr<CodeView> m_view;
        winrt::Microsoft::UI::Xaml::Controls::Primitives::ToggleButton m_wrapButton{nullptr};
    };
}
