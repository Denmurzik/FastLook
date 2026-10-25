#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Data.Pdf.h>

#include "Previewers/IPreviewer.h"
#include "Util/LruTracker.h"

namespace fastlook::viewer
{
    // PDF through Windows.Data.Pdf: the pages sit in an ItemsRepeater (vertical stack) inside a ScrollViewer as white placeholders
    // sized by the page aspect. A page that intersects the viewport is rendered at the page width x DPI scale (capped, see
    // below). Rendered pages are cached under two bounds: LruTracker keeps at most kMaxRenderedPages, and their pixels (plus
    // renders in flight) stay within kBudgetPixels. Only pages that are not on screen are ever evicted; when the visible pages
    // alone would not fit, each is rendered at a lower resolution instead. Zoom rebuilds the list and re-renders what is
    // visible; a window resize does the same once the size has settled.
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
        static constexpr double kPagePixels = 2e6;      // one page: 8 MB of BGRA
        static constexpr double kBudgetPixels = 8e6;    // all cached pages and renders in flight: 32 MB
        static constexpr double kSerialAbovePx = 1.5e6; // a bigger render runs alone (bounds the transient BMP + bitmap memory)
        static constexpr double kFloorPixels = 0.5e6;   // lowest resolution a visible page is squeezed to

    private:
        struct Rendered
        {
            winrt::Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource source{nullptr};
            uint64_t px = 0;
        };

        // Lets the UI thread cancel whichever WinRT operation a background render is awaiting.
        struct RenderJob
        {
            void Set(winrt::Windows::Foundation::IAsyncInfo const& operation)
            {
                std::lock_guard lock{mutex};
                current = operation;
                if (cancelled)
                {
                    operation.Cancel();
                }
            }
            void Cancel()
            {
                std::lock_guard lock{mutex};
                cancelled = true;
                if (current)
                {
                    current.Cancel();
                }
            }

            std::mutex mutex;
            winrt::Windows::Foundation::IAsyncInfo current{nullptr};
            bool cancelled = false;
        };

        struct Rendering
        {
            uint64_t id = 0;
            uint64_t px = 0; // reserved
            std::shared_ptr<RenderJob> job;
        };

        void BuildView();
        void Rebuild(bool keepPosition);
        void OnElementPrepared(winrt::Microsoft::UI::Xaml::Controls::ItemsRepeater const&,
                               winrt::Microsoft::UI::Xaml::Controls::ItemsRepeaterElementPreparedEventArgs const& args);
        void OnElementClearing(winrt::Microsoft::UI::Xaml::Controls::ItemsRepeater const&,
                               winrt::Microsoft::UI::Xaml::Controls::ItemsRepeaterElementClearingEventArgs const& args);
        void OnViewportChanged();
        void UpdateCounter();
        std::vector<uint32_t> VisibleIndices() const;
        void QueuePump();
        void PumpRenders();
        uint64_t Grantable(uint32_t index, uint64_t wanted, std::vector<uint32_t> const& visible) const;
        uint64_t Reserve(uint32_t index, uint64_t wanted, std::vector<uint32_t> const& visible);
        void ReleaseReservation(uint32_t index);
        void Evict(uint32_t index);
        void ClearRendered();
        uint64_t CachedPixels() const;
        double InnerWidthPx() const;
        double PageAspect(uint32_t index) const;
        uint32_t WidthForPixels(uint32_t index, uint64_t px) const;
        double PageHeightDip(uint32_t index) const;
        bool ScrollKey(winrt::Windows::System::VirtualKey key);
        winrt::fire_and_forget RenderPage(uint32_t index, uint64_t generation, uint64_t id, uint32_t widthPx,
                                          std::shared_ptr<RenderJob> job);

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

        core::LruTracker<uint32_t> m_lru{kMaxRenderedPages}; // pages that are rendered or being rendered
        std::map<uint32_t, Rendered> m_rendered;
        std::map<uint32_t, Rendering> m_rendering;
        std::set<uint32_t> m_failed; // pages whose render failed; not retried until the next rebuild
        std::map<uint32_t, winrt::Microsoft::UI::Xaml::Controls::Border> m_realized; // index -> page element

        double m_zoom = 1.0;
        double m_fitWidthDip = 0; // page width at zoom 1: the viewport width less the margins
        double m_pendingFitWidthDip = 0; // the latest viewport width, applied when the resize timer fires
        double m_pageWidthDip = 0;
        uint64_t m_generation = 0; // bumped by every rebuild and by Unload; stale renders drop their result
        uint64_t m_nextRenderId = 0;
        bool m_settled = false; // the window size stopped changing: pages may be rendered
        bool m_pumpQueued = false;
        bool m_loaded = false;
    };
}
