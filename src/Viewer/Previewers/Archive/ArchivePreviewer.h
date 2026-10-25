#pragma once

#include <memory>
#include <string>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "Archive/ArchiveListing.h"
#include "Previewers/IPreviewer.h"

namespace fastlook::viewer
{
    // The contents of an archive from its headers alone (Core ListArchive: nothing is extracted or decoded to disk).
    // PrepareAsync lists on a background thread within the Core limits (100 000 entries / 2 s) and formats nothing; View()
    // builds a summary line, optional notices (truncated / encrypted), a column header and the rows. The rows sit in an
    // ItemsRepeater inside a ScrollViewer, so only the visible ones exist: a row element is filled from the listing when the
    // repeater prepares it.
    class ArchivePreviewer final : public IPreviewer, public IKeyTarget, public ICommandTarget
    {
    public:
        winrt::Windows::Foundation::IAsyncAction PrepareAsync(PreviewItem item, std::shared_ptr<PreviewContext const> ctx) override;
        winrt::Microsoft::UI::Xaml::UIElement View() override;
        winrt::Windows::Foundation::Size PreferredSize() const override { return {720, 540}; }
        void Unload() override;

        bool OnKey(winrt::Windows::System::VirtualKey key, bool ctrl, bool shift) override;
        bool Execute(std::wstring_view command) override;

    private:
        void FillRow(winrt::Microsoft::UI::Xaml::Controls::Grid const& row, uint32_t index) const;
        bool ScrollKey(winrt::Windows::System::VirtualKey key);

        core::ArchiveListing m_listing;
        uint64_t m_archiveSize = 0;

        winrt::Microsoft::UI::Xaml::FrameworkElement m_root{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::ScrollViewer m_scroll{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::ItemsRepeater m_repeater{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::ItemsRepeater::ElementPrepared_revoker m_preparedRevoker;
        bool m_loaded = false;
    };
}
