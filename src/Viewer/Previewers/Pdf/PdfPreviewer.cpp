#include "pch.h"
#include "Previewers/Pdf/PdfPreviewer.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <winrt/Microsoft.UI.Xaml.Automation.h>

#include "Common/Plural.h"
#include "Diagnostics/Log.h"
#include "Diagnostics/PerfLog.h"
#include "Previewers/Common/ShellInfo.h"
#include "Previewers/Factories.h"
#include "Util/Async.h"
#include "Util/Str.h"

namespace fastlook::viewer
{
    namespace
    {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        using winrt::Microsoft::UI::Xaml::Media::SolidColorBrush;
        using winrt::Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource;
        using winrt::Windows::Data::Pdf::PdfDocument;
        using winrt::Windows::Data::Pdf::PdfPageRenderOptions;
        using winrt::Windows::Foundation::IInspectable;
        using winrt::Windows::Graphics::Imaging::BitmapAlphaMode;
        using winrt::Windows::Graphics::Imaging::BitmapDecoder;
        using winrt::Windows::Graphics::Imaging::BitmapEncoder;
        using winrt::Windows::Graphics::Imaging::BitmapPixelFormat;
        using winrt::Windows::Storage::StorageFile;
        using winrt::Windows::Storage::Streams::InMemoryRandomAccessStream;
        using winrt::Windows::UI::Color;

        constexpr double kMarginDip = 8;
        constexpr double kPageSpacingDip = 8;
        constexpr double kMinZoom = 0.5;
        constexpr double kMaxZoom = 4.0;
        constexpr double kZoomStep = 1.25;
        constexpr uint32_t kMaxRenderWidthPx = 4096;
        constexpr double kBorderDip = 1;
        constexpr auto kSizeProbeBudget = std::chrono::milliseconds(250); // measuring page sizes up front
        constexpr auto kRenderDelay = std::chrono::milliseconds(60);      // pages flown past while scrolling are never rendered
        constexpr auto kResizeDebounce = std::chrono::milliseconds(150);

        constexpr Color kWhite{0xFF, 0xFF, 0xFF, 0xFF};

        // The element the repeater gets for every page: a white sheet with the rendered image on it.
        struct PageFactory : winrt::implements<PageFactory, IElementFactory>
        {
            UIElement GetElement(ElementFactoryGetArgs const&)
            {
                Image image;
                image.Stretch(Media::Stretch::Uniform);
                Border sheet;
                sheet.Background(SolidColorBrush{kWhite});
                sheet.BorderBrush(SolidColorBrush{Color{0x40, 0x80, 0x80, 0x80}});
                sheet.BorderThickness(Thickness{1, 1, 1, 1});
                sheet.Child(image);
                return sheet;
            }

            void RecycleElement(ElementFactoryRecycleArgs const&) {}
        };

        std::wstring DocumentName(uint32_t pageCount)
        {
            bool const russian = Str(L"Pdf", L"PluralRule") == L"ru";
            auto const form = russian ? core::PluralRu(pageCount) : core::PluralEn(pageCount);
            wchar_t const* key = L"DocumentOther";
            switch (form)
            {
            case core::PluralForm::One:
                key = L"DocumentOne";
                break;
            case core::PluralForm::Few:
                key = L"DocumentFew";
                break;
            case core::PluralForm::Many:
                key = L"DocumentMany";
                break;
            case core::PluralForm::Other:
                break;
            }
            return FormatPattern(Str(L"Pdf", key), {FormatNumber(pageCount)});
        }

        void CloseQuietly(SoftwareBitmapSource const& source)
        {
            try
            {
                source.Close();
            }
            catch (...)
            {
            }
        }
    }

    winrt::Windows::Foundation::IAsyncAction PdfPreviewer::PrepareAsync(PreviewItem item, std::shared_ptr<PreviewContext const> ctx)
    {
        auto self = std::static_pointer_cast<PdfPreviewer>(shared_from_this());
        auto cancel = co_await winrt::get_cancellation_token();
        m_ctx = ctx;
        auto const ui = ctx->ui;
        std::wstring const path = item.ref.path;

        co_await winrt::resume_background();
        PdfDocument doc{nullptr};
        std::vector<winrt::Windows::Foundation::Size> sizes;
        bool passwordProtected = false;
        bool failed = false;
        try
        {
            auto file = co_await StorageFile::GetFileFromPathAsync(path);
            doc = co_await PdfDocument::LoadFromFileAsync(file);
            uint32_t const count = doc.PageCount();
            if (count == 0)
            {
                failed = true;
            }
            else
            {
                // Aspect of every page up front, within a time budget; the rest start as the first page and are corrected when rendered.
                auto const deadline = std::chrono::steady_clock::now() + kSizeProbeBudget;
                sizes.reserve(count);
                for (uint32_t i = 0; i < count && !cancel(); ++i)
                {
                    if (i > 0 && std::chrono::steady_clock::now() > deadline)
                    {
                        break;
                    }
                    auto page = doc.GetPage(i);
                    sizes.push_back(page.Size());
                    page.Close();
                }
                if (!sizes.empty())
                {
                    sizes.resize(count, sizes.front());
                }
            }
        }
        catch (winrt::hresult_error const& error)
        {
            if (cancel())
            {
                co_return;
            }
            passwordProtected = error.code() == HRESULT_FROM_WIN32(ERROR_WRONG_PASSWORD);
            failed = true;
            FL_LOG_INFO(L"Opening the PDF failed");
        }
        if (cancel())
        {
            co_return;
        }

        if (!co_await ResumeOn(ui) || cancel())
        {
            co_return;
        }
        if (failed || sizes.empty())
        {
            throw PreviewError{std::wstring{Str(L"Pdf", passwordProtected ? L"Protected" : L"OpenFailed")}, std::nullopt};
        }
        m_doc = doc;
        m_pageSizes = std::move(sizes);
        auto const first = m_pageSizes.front();
        m_preferred = {static_cast<float>(first.Width + 2 * kMarginDip), static_cast<float>(first.Height + 2 * kMarginDip)};
    }

    UIElement PdfPreviewer::View()
    {
        BuildView();
        return m_root;
    }

    void PdfPreviewer::BuildView()
    {
        auto weak = std::weak_ptr<PdfPreviewer>(std::static_pointer_cast<PdfPreviewer>(shared_from_this()));

        StackLayout layout;
        layout.Orientation(Orientation::Vertical);
        layout.Spacing(kPageSpacingDip);

        m_repeater = ItemsRepeater{};
        m_repeater.Layout(layout);
        m_repeater.ItemTemplate(winrt::make<PageFactory>());
        m_repeater.VerticalCacheLength(0.5);
        m_repeater.HorizontalCacheLength(0);
        m_repeater.Margin(Thickness{kMarginDip, kMarginDip, kMarginDip, kMarginDip});
        m_repeater.HorizontalAlignment(HorizontalAlignment::Center);
        m_repeater.VerticalAlignment(VerticalAlignment::Top);
        m_preparedRevoker = m_repeater.ElementPrepared(winrt::auto_revoke, [weak](auto const& sender, auto const& args) {
            if (auto self = weak.lock())
            {
                self->OnElementPrepared(sender, args);
            }
        });
        m_clearingRevoker = m_repeater.ElementClearing(winrt::auto_revoke, [weak](auto const& sender, auto const& args) {
            if (auto self = weak.lock())
            {
                self->OnElementClearing(sender, args);
            }
        });

        m_scroll = ScrollViewer{};
        m_scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Auto);
        m_scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        m_scroll.IsTabStop(false);
        m_scroll.Content(m_repeater);
        m_viewChangedRevoker = m_scroll.ViewChanged(winrt::auto_revoke, [weak](auto const&, auto const&) {
            if (auto self = weak.lock())
            {
                self->UpdateCounter();
                self->PumpRenders();
            }
        });
        m_sizeChangedRevoker = m_scroll.SizeChanged(winrt::auto_revoke, [weak](auto const&, auto const&) {
            if (auto self = weak.lock())
            {
                self->OnViewportChanged();
            }
        });

        m_counter = TextBlock{};
        m_counter.FontSize(12);
        m_counter.Foreground(SolidColorBrush{kWhite});
        m_counterBox = Border{};
        m_counterBox.Background(SolidColorBrush{Color{0xB0, 0x20, 0x20, 0x20}});
        m_counterBox.CornerRadius(CornerRadius{12, 12, 12, 12});
        m_counterBox.Padding(Thickness{12, 4, 12, 4});
        m_counterBox.Margin(Thickness{0, 0, 0, 12});
        m_counterBox.HorizontalAlignment(HorizontalAlignment::Center);
        m_counterBox.VerticalAlignment(VerticalAlignment::Bottom);
        m_counterBox.IsHitTestVisible(false);
        m_counterBox.Visibility(m_pageSizes.size() > 1 ? Visibility::Visible : Visibility::Collapsed);
        m_counterBox.Child(m_counter);

        Grid root;
        root.Children().Append(m_scroll);
        root.Children().Append(m_counterBox);
        Automation::AutomationProperties::SetName(root, winrt::hstring{DocumentName(static_cast<uint32_t>(m_pageSizes.size()))});
        m_root = root;

        m_resizeTimer = m_ctx->ui.CreateTimer();
        m_resizeTimer.Interval(kResizeDebounce);
        m_resizeTimer.IsRepeating(false);
        m_resizeTimer.Tick([weak](auto const&, auto const&) {
            if (auto self = weak.lock(); self && self->m_loaded)
            {
                if (std::abs(self->m_pendingFitWidthDip - self->m_fitWidthDip) >= 1)
                {
                    self->m_fitWidthDip = self->m_pendingFitWidthDip;
                    self->Rebuild(true);
                }
                self->m_settled = true;
                self->PumpRenders();
            }
        });

        m_loaded = true;
        m_settled = false;
        m_fitWidthDip = 0;
        m_zoom = 1.0;
    }

    void PdfPreviewer::OnViewportChanged()
    {
        if (!m_loaded)
        {
            return;
        }
        double const width = m_scroll.ActualWidth() - 2 * kMarginDip;
        if (width < 1)
        {
            return;
        }
        m_pendingFitWidthDip = width;
        if (m_fitWidthDip == 0)
        {
            // Placeholders at once, but no renders until the window size has settled (it is still animating to its fitted size).
            m_fitWidthDip = width;
            Rebuild(false);
        }
        if (!m_settled || std::abs(width - m_fitWidthDip) >= 1)
        {
            m_settled = false;
            m_resizeTimer.Stop();
            m_resizeTimer.Start();
        }
    }

    void PdfPreviewer::Rebuild(bool keepPosition)
    {
        ++m_generation;
        ClearRendered();

        double verticalCentre = 0; // fraction of the extent at the viewport centre
        double horizontalCentre = 0.5;
        if (keepPosition && m_scroll.ExtentHeight() > 0 && m_scroll.ExtentWidth() > 0)
        {
            verticalCentre = (m_scroll.VerticalOffset() + m_scroll.ViewportHeight() / 2) / m_scroll.ExtentHeight();
            horizontalCentre = (m_scroll.HorizontalOffset() + m_scroll.ViewportWidth() / 2) / m_scroll.ExtentWidth();
        }

        m_pageWidthDip = m_fitWidthDip * m_zoom;
        m_repeater.Width(m_pageWidthDip);
        auto items = winrt::single_threaded_vector<IInspectable>();
        for (uint32_t i = 0; i < m_pageSizes.size(); ++i)
        {
            items.Append(winrt::box_value(i));
        }
        m_repeater.ItemsSource(items);

        if (keepPosition)
        {
            m_scroll.UpdateLayout();
            double const vertical = std::max(0.0, verticalCentre * m_scroll.ExtentHeight() - m_scroll.ViewportHeight() / 2);
            double const horizontal = std::max(0.0, horizontalCentre * m_scroll.ExtentWidth() - m_scroll.ViewportWidth() / 2);
            m_scroll.ChangeView(horizontal, vertical, nullptr, true);
        }
        UpdateCounter();
        QueuePump();
    }

    // The sheet is page-shaped inside its 1 px border.
    double PdfPreviewer::PageHeightDip(uint32_t index) const
    {
        return std::max(0.0, m_pageWidthDip - 2 * kBorderDip) * PageAspect(index) + 2 * kBorderDip;
    }

    double PdfPreviewer::PageAspect(uint32_t index) const
    {
        auto const size = m_pageSizes[index];
        return size.Width > 0 ? size.Height / size.Width : 1.0;
    }

    double PdfPreviewer::InnerWidthPx() const
    {
        return std::max(1.0, (m_pageWidthDip - 2 * kBorderDip) * m_ctx->scale);
    }

    // The widest render of the page that fits in `px` pixels, never wider than the page is shown.
    uint32_t PdfPreviewer::WidthForPixels(uint32_t index, uint64_t px) const
    {
        double const aspect = std::max(PageAspect(index), 0.01);
        double const width = std::min({InnerWidthPx(), static_cast<double>(kMaxRenderWidthPx), std::sqrt(static_cast<double>(px) / aspect)});
        return static_cast<uint32_t>(std::max(1.0, std::round(width)));
    }

    uint64_t PdfPreviewer::CachedPixels() const
    {
        uint64_t total = 0;
        for (auto const& [index, rendered] : m_rendered)
        {
            total += rendered.px;
        }
        for (auto const& [index, rendering] : m_rendering)
        {
            total += rendering.px;
        }
        return total;
    }

    void PdfPreviewer::OnElementPrepared(ItemsRepeater const&, ItemsRepeaterElementPreparedEventArgs const& args)
    {
        auto const index = static_cast<uint32_t>(args.Index());
        if (!m_loaded || index >= m_pageSizes.size())
        {
            return;
        }
        auto sheet = args.Element().as<Border>();
        sheet.Height(PageHeightDip(index));
        m_realized[index] = sheet;

        auto image = sheet.Child().as<Image>();
        if (auto it = m_rendered.find(index); it != m_rendered.end())
        {
            image.Source(it->second.source);
            m_lru.Touch(index);
        }
        else
        {
            image.Source(nullptr);
        }
        QueuePump(); // visibility needs the arranged position: after this layout pass
    }

    void PdfPreviewer::OnElementClearing(ItemsRepeater const&, ItemsRepeaterElementClearingEventArgs const& args)
    {
        auto sheet = args.Element().try_as<Border>();
        if (!sheet)
        {
            return;
        }
        for (auto it = m_realized.begin(); it != m_realized.end(); ++it)
        {
            if (it->second == sheet)
            {
                m_realized.erase(it);
                break;
            }
        }
        if (auto image = sheet.Child().try_as<Image>())
        {
            image.Source(nullptr); // the rendered page stays cached until it is evicted
        }
    }

    // Realized pages that intersect the viewport, ascending.
    std::vector<uint32_t> PdfPreviewer::VisibleIndices() const
    {
        std::vector<uint32_t> visible;
        double const height = m_scroll.ActualHeight();
        for (auto const& [index, sheet] : m_realized)
        {
            try
            {
                double const top = sheet.TransformToVisual(m_scroll).TransformPoint({0, 0}).Y;
                if (top < height && top + sheet.Height() > 0)
                {
                    visible.push_back(index);
                }
            }
            catch (...)
            {
            }
        }
        return visible;
    }

    void PdfPreviewer::QueuePump()
    {
        if (m_pumpQueued || !m_loaded)
        {
            return;
        }
        m_pumpQueued = true;
        m_ctx->ui.TryEnqueue([weak = std::weak_ptr<PdfPreviewer>(std::static_pointer_cast<PdfPreviewer>(shared_from_this()))] {
            if (auto self = weak.lock())
            {
                self->m_pumpQueued = false;
                if (self->m_loaded)
                {
                    self->UpdateCounter();
                    self->PumpRenders();
                }
            }
        });
    }

    // Starts the renders for visible pages that lack an image. Never evicts a visible page; squeezes the resolution instead.
    void PdfPreviewer::PumpRenders()
    {
        if (!m_loaded || !m_settled || !m_scroll)
        {
            return;
        }
        auto const visible = VisibleIndices();
        auto const isVisible = [&visible](uint32_t index) { return std::binary_search(visible.begin(), visible.end(), index); };

        // A render for a page that has been scrolled away is not worth finishing.
        for (auto it = m_rendering.begin(); it != m_rendering.end();)
        {
            if (!isVisible(it->first))
            {
                it->second.job->Cancel();
                if (!m_rendered.contains(it->first))
                {
                    m_lru.Erase(it->first);
                }
                it = m_rendering.erase(it);
            }
            else
            {
                ++it;
            }
        }

        double const share = std::min(kPagePixels, kBudgetPixels / static_cast<double>(std::max<size_t>(1, visible.size())));
        for (uint32_t const index : visible)
        {
            if (m_failed.contains(index))
            {
                continue;
            }
            uint64_t const natural = static_cast<uint64_t>(InnerWidthPx() * InnerWidthPx() * PageAspect(index));
            uint64_t wanted = std::min<uint64_t>(static_cast<uint64_t>(share), std::max<uint64_t>(natural, 1));
            auto const rendered = m_rendered.find(index);
            if (m_rendering.contains(index))
            {
                m_lru.Touch(index);
                continue;
            }
            if (rendered != m_rendered.end())
            {
                m_lru.Touch(index);
                // Squeezed earlier, and the visible count has dropped since: re-render sharper (the old image stays until then).
                if (rendered->second.px * 5 >= wanted * 4)
                {
                    continue;
                }
            }
            if (m_rendering.size() >= (wanted > kSerialAbovePx ? 1u : 2u))
            {
                break; // the next finish pumps again
            }
            uint64_t const previousPx = rendered != m_rendered.end() ? rendered->second.px : 0;
            wanted = Reserve(index, wanted, visible);
            if (previousPx != 0 && wanted * 4 < previousPx * 5)
            {
                continue; // the budget still allows no sharper image
            }
            uint32_t const widthPx = WidthForPixels(index, wanted);
            auto job = std::make_shared<RenderJob>();
            uint64_t const id = ++m_nextRenderId;
            m_rendering[index] = Rendering{id, wanted, job};
            RenderPage(index, m_generation, id, widthPx, job);
        }
    }

    // Makes room for a render of up to `wanted` pixels by evicting pages that are not visible; returns the pixels granted
    // (less than wanted when the visible pages alone fill the budget).
    uint64_t PdfPreviewer::Reserve(uint32_t index, uint64_t wanted, std::vector<uint32_t> const& visible)
    {
        auto const isVisible = [&visible](uint32_t i) { return std::binary_search(visible.begin(), visible.end(), i); };
        if (auto const evicted = m_lru.TouchProtected(index, isVisible))
        {
            Evict(*evicted);
        }
        auto const budget = static_cast<uint64_t>(kBudgetPixels);
        auto const own = m_rendered.contains(index) ? m_rendered.at(index).px : 0; // replaced by the new image
        auto const cached = [&] { return CachedPixels() - own; };
        while (cached() + wanted > budget)
        {
            auto const victim = m_lru.OldestIf([&](uint32_t k) { return k != index && !isVisible(k); });
            if (!victim)
            {
                break;
            }
            Evict(*victim);
        }
        uint64_t const used = cached();
        if (used + wanted > budget)
        {
            wanted = std::max<uint64_t>(static_cast<uint64_t>(kFloorPixels), budget > used ? budget - used : 0);
        }
        return wanted;
    }

    // Drops the render in flight for the page; the page leaves the LRU unless an older image of it is still cached.
    void PdfPreviewer::ReleaseReservation(uint32_t index)
    {
        m_rendering.erase(index);
        if (!m_rendered.contains(index))
        {
            m_lru.Erase(index);
        }
    }

    void PdfPreviewer::Evict(uint32_t index)
    {
        m_lru.Erase(index);
        if (auto realized = m_realized.find(index); realized != m_realized.end())
        {
            if (auto image = realized->second.Child().try_as<Image>())
            {
                image.Source(nullptr);
            }
        }
        if (auto rendering = m_rendering.find(index); rendering != m_rendering.end())
        {
            rendering->second.job->Cancel();
            m_rendering.erase(rendering);
        }
        if (auto it = m_rendered.find(index); it != m_rendered.end())
        {
            CloseQuietly(it->second.source);
            m_rendered.erase(it);
        }
    }

    void PdfPreviewer::ClearRendered()
    {
        for (auto& [index, sheet] : m_realized)
        {
            if (auto image = sheet.Child().try_as<Image>())
            {
                image.Source(nullptr);
            }
        }
        for (auto& [index, rendered] : m_rendered)
        {
            CloseQuietly(rendered.source);
        }
        for (auto& [index, rendering] : m_rendering)
        {
            rendering.job->Cancel();
        }
        m_rendered.clear();
        m_rendering.clear();
        m_failed.clear();
        m_lru.Clear();
    }

    winrt::fire_and_forget PdfPreviewer::RenderPage(uint32_t index, uint64_t generation, uint64_t id, uint32_t widthPx,
                                                    std::shared_ptr<RenderJob> job)
    {
        auto self = std::static_pointer_cast<PdfPreviewer>(shared_from_this());
        auto const ui = m_ctx->ui;
        auto const doc = m_doc;
        // UI thread only. Still the render the previewer is waiting for (not evicted, cancelled or superseded)?
        auto const current = [&self, index, generation, id] {
            if (generation != self->m_generation || !self->m_loaded)
            {
                return false;
            }
            auto it = self->m_rendering.find(index);
            return it != self->m_rendering.end() && it->second.id == id;
        };
        // UI thread only: gives up this render's reservation and lets the next page start.
        auto const abandon = [&self, index, &current] {
            if (current())
            {
                self->ReleaseReservation(index);
                self->PumpRenders();
            }
        };
        try
        {
            co_await winrt::resume_after(kRenderDelay);
            if (!co_await ResumeOn(ui))
            {
                co_return;
            }
            // Superseded, or scrolled out of view during the delay: nothing to render.
            if (!current())
            {
                co_return;
            }
            auto const stillVisible = self->VisibleIndices();
            if (!std::binary_search(stillVisible.begin(), stillVisible.end(), index))
            {
                abandon();
                co_return;
            }

            co_await winrt::resume_background();
            auto page = doc.GetPage(index);
            auto const pageSize = page.Size();
            PdfPageRenderOptions options;
            options.DestinationWidth(widthPx);
            options.BackgroundColor(kWhite);
            options.BitmapEncoderId(BitmapEncoder::BmpEncoderId()); // uncompressed: no PNG encode/decode round trip
            InMemoryRandomAccessStream stream;
            auto render = page.RenderToStreamAsync(stream, options);
            job->Set(render);
            co_await render;
            auto openDecoder = BitmapDecoder::CreateAsync(stream);
            job->Set(openDecoder);
            auto decoder = co_await openDecoder;
            auto convert = decoder.GetSoftwareBitmapAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied);
            job->Set(convert);
            auto bitmap = co_await convert;
            stream.Close();
            page.Close();

            if (!co_await ResumeOn(ui))
            {
                bitmap.Close();
                co_return;
            }
            // A page the up-front probe did not reach: keep its real size even when it is no longer wanted.
            if (self->m_loaded && index < self->m_pageSizes.size() &&
                (std::abs(self->m_pageSizes[index].Width - pageSize.Width) > 0.5 ||
                 std::abs(self->m_pageSizes[index].Height - pageSize.Height) > 0.5))
            {
                self->m_pageSizes[index] = pageSize;
                if (auto realized = self->m_realized.find(index); realized != self->m_realized.end())
                {
                    realized->second.Height(self->PageHeightDip(index));
                }
            }
            if (!current())
            {
                bitmap.Close();
                co_return;
            }
            uint64_t const px = static_cast<uint64_t>(bitmap.PixelWidth()) * static_cast<uint64_t>(bitmap.PixelHeight());
            SoftwareBitmapSource source;
            co_await source.SetBitmapAsync(bitmap);
            bitmap.Close();
            if (!current())
            {
                CloseQuietly(source);
                co_return;
            }

            self->m_rendering.erase(index);
            if (auto old = self->m_rendered.find(index); old != self->m_rendered.end())
            {
                CloseQuietly(old->second.source);
                old->second = Rendered{source, px};
            }
            else
            {
                self->m_rendered.emplace(index, Rendered{source, px});
            }
            core::perf::Mark("pdf-page-rendered", "page=" + std::to_string(index) + " w=" + std::to_string(widthPx));
            if (auto realized = self->m_realized.find(index); realized != self->m_realized.end())
            {
                if (auto image = realized->second.Child().try_as<Image>())
                {
                    image.Source(source);
                }
            }
            self->PumpRenders();
        }
        catch (winrt::hresult_canceled const&)
        {
            // Cancelled: normally by an eviction, a rebuild or Unload, which already dropped the reservation. If the operation
            // was cancelled from outside, release it here so it can never pin budget; the page may render again.
            ui.TryEnqueue([self, index, generation, id] {
                if (generation == self->m_generation && self->m_loaded)
                {
                    if (auto it = self->m_rendering.find(index); it != self->m_rendering.end() && it->second.id == id)
                    {
                        self->ReleaseReservation(index);
                        self->PumpRenders();
                    }
                }
            });
        }
        catch (...)
        {
            FL_LOG_INFO(L"Rendering a PDF page failed");
            // The failure may have happened off the UI thread: give up the reservation there.
            ui.TryEnqueue([self, index, generation, id] {
                if (generation == self->m_generation && self->m_loaded)
                {
                    if (auto it = self->m_rendering.find(index); it != self->m_rendering.end() && it->second.id == id)
                    {
                        self->ReleaseReservation(index);
                        self->m_failed.insert(index);
                        self->PumpRenders();
                    }
                }
            });
        }
    }

    void PdfPreviewer::UpdateCounter()
    {
        if (!m_loaded || m_pageSizes.size() <= 1)
        {
            return;
        }
        double const centre = m_scroll.ActualHeight() / 2;
        std::optional<uint32_t> current;
        for (auto const& [index, sheet] : m_realized)
        {
            double const top = sheet.TransformToVisual(m_scroll).TransformPoint({0, 0}).Y;
            if (top <= centre && centre <= top + sheet.Height())
            {
                current = index;
                break;
            }
        }
        if (!current)
        {
            return; // between two pages, or nothing laid out yet: keep the text
        }
        m_counter.Text(winrt::hstring{
            FormatPattern(Str(L"Pdf", L"PageCounter"), {FormatNumber(*current + 1), FormatNumber(m_pageSizes.size())})});
    }

    void PdfPreviewer::Zoom(ZoomCommand command)
    {
        if (!m_loaded)
        {
            return;
        }
        double zoom = m_zoom;
        switch (command)
        {
        case ZoomCommand::In:
            zoom = std::min(kMaxZoom, m_zoom * kZoomStep);
            break;
        case ZoomCommand::Out:
            zoom = std::max(kMinZoom, m_zoom / kZoomStep);
            break;
        case ZoomCommand::Reset:
            zoom = 1.0;
            break;
        }
        if (std::abs(zoom - m_zoom) < 1e-6 || m_fitWidthDip == 0)
        {
            return;
        }
        m_zoom = zoom;
        Rebuild(true);
    }

    bool PdfPreviewer::OnKey(winrt::Windows::System::VirtualKey key, bool ctrl, bool shift)
    {
        if (ctrl || shift)
        {
            return false;
        }
        return ScrollKey(key);
    }

    bool PdfPreviewer::Execute(std::wstring_view command)
    {
        using winrt::Windows::System::VirtualKey;
        // Scrolling by script (UI verification).
        if (command == L"pageup")
        {
            return ScrollKey(VirtualKey::PageUp);
        }
        if (command == L"pagedown")
        {
            return ScrollKey(VirtualKey::PageDown);
        }
        if (command == L"home")
        {
            return ScrollKey(VirtualKey::Home);
        }
        if (command == L"end")
        {
            return ScrollKey(VirtualKey::End);
        }
        return false;
    }

    bool PdfPreviewer::ScrollKey(winrt::Windows::System::VirtualKey key)
    {
        using winrt::Windows::System::VirtualKey;
        if (!m_loaded)
        {
            return false;
        }
        double const page = m_scroll.ViewportHeight() * 0.9;
        double target = m_scroll.VerticalOffset();
        switch (key)
        {
        case VirtualKey::PageUp:
            target -= page;
            break;
        case VirtualKey::PageDown:
            target += page;
            break;
        case VirtualKey::Home:
            target = 0;
            break;
        case VirtualKey::End:
            target = m_scroll.ScrollableHeight();
            break;
        default:
            return false;
        }
        m_scroll.ChangeView(nullptr, std::max(0.0, target), nullptr, true);
        return true;
    }

    void PdfPreviewer::Unload()
    {
        ++m_generation;
        m_loaded = false;
        if (m_resizeTimer)
        {
            m_resizeTimer.Stop();
            m_resizeTimer = nullptr;
        }
        m_preparedRevoker.revoke();
        m_clearingRevoker.revoke();
        m_viewChangedRevoker.revoke();
        m_sizeChangedRevoker.revoke();
        ClearRendered();
        m_realized.clear();
        if (m_repeater)
        {
            m_repeater.ItemsSource(nullptr);
        }
        m_repeater = nullptr;
        m_scroll = nullptr;
        m_counter = nullptr;
        m_counterBox = nullptr;
        m_root = nullptr;
        m_doc = nullptr;
        m_pageSizes.clear();
    }

    std::shared_ptr<IPreviewer> CreatePdfPreviewer()
    {
        return std::make_shared<PdfPreviewer>();
    }
}
