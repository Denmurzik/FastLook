#pragma once

#include <deque>
#include <map>
#include <set>
#include <memory>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Data.Pdf.h>

#include "Previewers/IPreviewer.h"
#include "Util/LruTracker.h"

namespace fastlook::viewer
{
    // PDF through Windows.Data.Pdf: the pages sit in an ItemsRepeater (vertical stack) inside a ScrollViewer as white placeholders
    // sized by the page aspect. A page is rendered, at the page width x DPI scale, when the repeater realizes it, and
    // LruTracker keeps at most kMaxRenderedPages rendered (the least recent one is closed). Zoom rebuilds the list and re-renders
    // what is visible; a window resize does the same after a short pause.
    class PdfPreviewer final : public IPreviewer, public IZoomable, public IKeyTarget, public ICommandTarget
    {
    public:
        winrt::Windows::Foundation::IAsyncAction PrepareAsync(PreviewItem item, std::shared_ptr<PreviewContext const> ctx) override;
        winrt::Microsoft::UI::Xaml::UIElement View() override;
        winrt::Windows::Foundation::Size PreferredSize() const override { return m_preferred; }
        bool KeepAspect() const override { return true; }
        void Unload() override;

        void Zoom(ZoomCommand command) override;
        bool OnKey(winrt::Windows::System::VirtualKey key, bool ctrl, bool shift) override;
        bool Execute(std::wstring_view command) override;

        static constexpr size_t kMaxRenderedPages = 6;
        static constexpr size_t kMaxConcurrentRenders = 2; // bounds the transient BMP + bitmap memory

    private:
        struct Rendered
        {
            winrt::Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource source{nullptr};
        };

        void BuildView();
        void Rebuild(bool keepPosition);
        void OnElementPrepared(winrt::Microsoft::UI::Xaml::Controls::ItemsRepeater const&,
                               winrt::Microsoft::UI::Xaml::Controls::ItemsRepeaterElementPreparedEventArgs const& args);
        void OnElementClearing(winrt::Microsoft::UI::Xaml::Controls::ItemsRepeater const&,
                               winrt::Microsoft::UI::Xaml::Controls::ItemsRepeaterElementClearingEventArgs const& args);
        void OnViewportChanged();
        void UpdateCounter();
        void Evict(uint32_t index);
        void ClearRendered();
        void PumpRenders();
        uint32_t TargetWidthPx(uint32_t index) const;
        double PageHeightDip(uint32_t index) const;
        bool ScrollKey(winrt::Windows::System::VirtualKey key);
        winrt::fire_and_forget RenderPage(uint32_t index, uint64_t generation, uint32_t widthPx);

        std::shared_ptr<PreviewContext const> m_ctx;
        winrt::Windows::Data::Pdf::PdfDocument m_doc{nullptr};
        std::vector<winrt::Windows::Foundation::Size> m_pageSizes; // DIP; pages not measured up front use the first page's size
        winrt::Windows::Foundation::Size m_preferred{};

        winrt::Microsoft::UI::Xaml::FrameworkElement m_root{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::ScrollViewer m_scroll{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::ItemsRepeater m_repeater{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_counter{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::Border m_counterBox{nullptr};
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_resizeTimer{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::ItemsRepeater::ElementPrepared_revoker m_preparedRevoker;
        winrt::Microsoft::UI::Xaml::Controls::ItemsRepeater::ElementClearing_revoker m_clearingRevoker;
        winrt::Microsoft::UI::Xaml::Controls::ScrollViewer::ViewChanged_revoker m_viewChangedRevoker;
        winrt::Microsoft::UI::Xaml::FrameworkElement::SizeChanged_revoker m_sizeChangedRevoker;

        core::LruTracker<uint32_t> m_lru{kMaxRenderedPages};
        std::map<uint32_t, Rendered> m_rendered;
        std::set<uint32_t> m_rendering; // renders in flight for the current generation
        std::deque<uint32_t> m_waiting; // realized pages waiting for a render slot
        std::map<uint32_t, winrt::Microsoft::UI::Xaml::Controls::Border> m_realized; // index -> page element

        double m_zoom = 1.0;
        double m_fitWidthDip = 0; // page width at zoom 1: the viewport width less the margins
        double m_pendingFitWidthDip = 0; // a resize waiting for its debounce timer
        double m_pageWidthDip = 0;
        uint64_t m_generation = 0; // bumped by every rebuild and by Unload; stale renders drop their result
        bool m_loaded = false;
    };
}
