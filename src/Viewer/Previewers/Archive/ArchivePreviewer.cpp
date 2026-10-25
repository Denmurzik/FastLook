#include "pch.h"
#include "Previewers/Archive/ArchivePreviewer.h"

#include <algorithm>
#include <stop_token>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Text.h>

#include "Common/Plural.h"
#include "Diagnostics/Log.h"
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
        using winrt::Windows::Foundation::IInspectable;
        using winrt::Windows::UI::Color;

        constexpr double kRowHeightDip = 28;
        constexpr double kGlyphColumnDip = 28;
        constexpr double kSizeColumnDip = 90;
        constexpr double kModifiedColumnDip = 150;
        constexpr wchar_t const* kFolderGlyph = L"";
        constexpr wchar_t const* kFileGlyph = L"";
        constexpr int64_t kFirstUnixSecondOf10000 = 253402300800; // 10000-01-01: the FILETIME range ends before it

        void SetColumns(Grid const& grid)
        {
            auto columns = grid.ColumnDefinitions();
            auto add = [&columns](GridLength length) {
                ColumnDefinition column;
                column.Width(length);
                columns.Append(column);
            };
            add(GridLength{kGlyphColumnDip, GridUnitType::Pixel});
            add(GridLength{1, GridUnitType::Star});
            add(GridLength{kSizeColumnDip, GridUnitType::Pixel});
            add(GridLength{kSizeColumnDip, GridUnitType::Pixel});
            add(GridLength{kModifiedColumnDip, GridUnitType::Pixel});
            grid.Padding(Thickness{8, 0, 8, 0});
        }

        TextBlock MakeCell(int column, bool right)
        {
            TextBlock text;
            text.VerticalAlignment(VerticalAlignment::Center);
            text.TextTrimming(TextTrimming::CharacterEllipsis);
            text.TextWrapping(TextWrapping::NoWrap);
            if (right)
            {
                text.HorizontalAlignment(HorizontalAlignment::Right);
                text.Opacity(0.8);
            }
            else if (column == 4)
            {
                text.Opacity(0.8);
                text.Margin(Thickness{16, 0, 0, 0});
            }
            Grid::SetColumn(text, column);
            return text;
        }

        // The element the repeater gets for every entry: glyph, name, size, packed size, modified. Texts are set when it is prepared.
        struct RowFactory : winrt::implements<RowFactory, IElementFactory>
        {
            UIElement GetElement(ElementFactoryGetArgs const&)
            {
                if (!m_pool.empty())
                {
                    UIElement reused = std::move(m_pool.back());
                    m_pool.pop_back();
                    return reused;
                }
                Grid row;
                row.Height(kRowHeightDip);
                SetColumns(row);

                FontIcon icon;
                icon.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily{L"Segoe Fluent Icons, Segoe MDL2 Assets"});
                icon.FontSize(14);
                icon.HorizontalAlignment(HorizontalAlignment::Left);
                icon.VerticalAlignment(VerticalAlignment::Center);
                Grid::SetColumn(icon, 0);
                row.Children().Append(icon);
                row.Children().Append(MakeCell(1, false));
                row.Children().Append(MakeCell(2, true));
                row.Children().Append(MakeCell(3, true));
                row.Children().Append(MakeCell(4, false));
                return row;
            }

            // Rows leaving the viewport are kept (a few) and refilled by the prepare event: a fast scroll builds no new elements.
            void RecycleElement(ElementFactoryRecycleArgs const& args)
            {
                if (m_pool.size() < kRowPoolSize)
                {
                    m_pool.push_back(args.Element());
                }
            }

        private:
            static constexpr size_t kRowPoolSize = 64;
            std::vector<UIElement> m_pool;
        };

        // Singular / few / many / other by the UI language (the Archive.resw PluralRule).
        std::wstring Counted(std::wstring_view base, uint64_t n)
        {
            bool const russian = Str(L"Archive", L"PluralRule") == L"ru";
            auto const form = russian ? core::PluralRu(n) : core::PluralEn(n);
            wchar_t const* suffix = L"Other";
            switch (form)
            {
            case core::PluralForm::One:
                suffix = L"One";
                break;
            case core::PluralForm::Few:
                suffix = L"Few";
                break;
            case core::PluralForm::Many:
                suffix = L"Many";
                break;
            case core::PluralForm::Other:
                break;
            }
            return FormatPattern(Str(L"Archive", std::wstring{base} + suffix), {FormatNumber(n)});
        }

        std::wstring FormatUnixTime(int64_t seconds)
        {
            if (seconds <= 0 || seconds >= kFirstUnixSecondOf10000)
            {
                return {};
            }
            ULARGE_INTEGER ticks;
            ticks.QuadPart = static_cast<uint64_t>(seconds + 11644473600LL) * 10000000ull;
            FILETIME time{ticks.LowPart, ticks.HighPart};
            return FormatDate(time);
        }

        TextBlock MakeHeaderCell(winrt::hstring const& text, int column, bool right)
        {
            TextBlock cell = MakeCell(column, right);
            cell.Text(text);
            cell.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            return cell;
        }

        Border MakeNotice(std::wstring const& message)
        {
            TextBlock text;
            text.Text(winrt::hstring{message});
            text.TextWrapping(TextWrapping::Wrap);
            Border notice;
            notice.Background(SolidColorBrush{Color{0x30, 0x80, 0x80, 0x80}});
            notice.CornerRadius(CornerRadius{4, 4, 4, 4});
            notice.Padding(Thickness{12, 6, 12, 6});
            notice.Margin(Thickness{12, 0, 12, 8});
            notice.Child(text);
            return notice;
        }
    }

    winrt::Windows::Foundation::IAsyncAction ArchivePreviewer::PrepareAsync(PreviewItem item, std::shared_ptr<PreviewContext const> ctx)
    {
        auto self = std::static_pointer_cast<ArchivePreviewer>(shared_from_this());
        auto cancel = co_await winrt::get_cancellation_token();
        auto const ui = ctx->ui;
        if (item.cloudPlaceholder || item.ref.isVirtual)
        {
            // Decision 56: a cloud placeholder is never read; a virtual item has no file to open.
            throw PreviewError{std::wstring{Str(L"Archive", L"OpenFailed")}, std::nullopt};
        }
        std::wstring const path = item.ref.path;

        // The WinRT cancellation reaches the Core listing as a stop token (checked in every read of the archive).
        auto stop = std::make_shared<std::stop_source>();
        cancel.callback([stop] { stop->request_stop(); });

        co_await winrt::resume_background();
        core::ArchiveListing listing = core::ListArchive(path, {}, stop->get_token());
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        uint64_t archiveSize = 0;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
        {
            archiveSize = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
        }
        if (cancel())
        {
            co_return;
        }

        if (!co_await ResumeOn(ui) || cancel())
        {
            co_return;
        }
        if (listing.headerTooLarge)
        {
            throw PreviewError{std::wstring{Str(L"Archive", L"HeaderTooLarge")}, std::nullopt};
        }
        if (listing.entries.empty() && listing.truncated)
        {
            // The time limit ran out before the first entry: "first 0 entries" would say nothing.
            throw PreviewError{std::wstring{Str(L"Archive", L"TooSlow")}, std::nullopt};
        }
        if (listing.entries.empty() && !listing.encryptedHeaders && !listing.error.empty())
        {
            FL_LOG_INFO(L"Listing the archive failed");
            throw PreviewError{std::wstring{Str(L"Archive", L"OpenFailed")}, std::nullopt};
        }
        m_listing = std::move(listing);
        m_archiveSize = archiveSize;
    }

    UIElement ArchivePreviewer::View()
    {
        m_loaded = true;
        auto weak = std::weak_ptr<ArchivePreviewer>(std::static_pointer_cast<ArchivePreviewer>(shared_from_this()));

        Grid root;
        if (m_listing.encryptedHeaders)
        {
            // The names cannot be read without the password: nothing to list.
            TextBlock message;
            message.Text(Str(L"Archive", L"Encrypted"));
            message.FontSize(20);
            message.TextWrapping(TextWrapping::Wrap);
            message.HorizontalAlignment(HorizontalAlignment::Center);
            message.VerticalAlignment(VerticalAlignment::Center);
            message.Margin(Thickness{24, 24, 24, 24});
            root.Children().Append(message);
            winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(root, Str(L"Archive", L"Encrypted"));
            m_root = root;
            return m_root;
        }

        for (int i = 0; i < 4; ++i)
        {
            RowDefinition row;
            row.Height(i == 3 ? GridLength{1, GridUnitType::Star} : GridLength{0, GridUnitType::Auto});
            root.RowDefinitions().Append(row);
        }

        // Summary line.
        bool const sizeKnown = std::all_of(m_listing.entries.begin(), m_listing.entries.end(), [](auto const& e) { return e.sizeKnown; });
        std::wstring const files = Counted(L"Files", m_listing.fileCount);
        std::wstring const folders = Counted(L"Folders", m_listing.dirCount);
        std::wstring const summaryText =
            sizeKnown ? FormatPattern(Str(L"Archive", L"Summary"), {files, folders, FormatBytes(m_listing.totalSize), FormatBytes(m_archiveSize)})
                      : FormatPattern(Str(L"Archive", L"SummaryNoSize"), {files, folders, FormatBytes(m_archiveSize)});
        TextBlock summary;
        summary.Text(winrt::hstring{summaryText});
        summary.TextWrapping(TextWrapping::Wrap);
        summary.Margin(Thickness{16, 12, 16, 8});
        Grid::SetRow(summary, 0);
        root.Children().Append(summary);

        // Notices.
        StackPanel notices;
        if (m_listing.truncated)
        {
            notices.Children().Append(MakeNotice(Counted(L"Truncated", m_listing.entries.size())));
        }
        if (std::any_of(m_listing.entries.begin(), m_listing.entries.end(), [](auto const& e) { return e.encrypted; }))
        {
            notices.Children().Append(MakeNotice(std::wstring{Str(L"Archive", L"Encrypted")}));
        }
        Grid::SetRow(notices, 1);
        root.Children().Append(notices);

        // Column header.
        Grid header;
        SetColumns(header);
        header.Children().Append(MakeHeaderCell(Str(L"Archive", L"Name"), 1, false));
        header.Children().Append(MakeHeaderCell(Str(L"Archive", L"Size"), 2, true));
        header.Children().Append(MakeHeaderCell(Str(L"Archive", L"Packed"), 3, true));
        header.Children().Append(MakeHeaderCell(Str(L"Archive", L"Modified"), 4, false));
        Border headerBox;
        headerBox.BorderBrush(SolidColorBrush{Color{0x40, 0x80, 0x80, 0x80}});
        headerBox.BorderThickness(Thickness{0, 0, 0, 1});
        headerBox.Padding(Thickness{0, 0, 0, 4});
        headerBox.Child(header);
        Grid::SetRow(headerBox, 2);
        root.Children().Append(headerBox);

        // Rows. The repeater only realises the ones in (and a little around) the viewport; every item is the same boxed value,
        // the row index comes from the prepare event.
        StackLayout layout;
        layout.Orientation(Orientation::Vertical);
        layout.Spacing(0);
        m_repeater = ItemsRepeater{};
        m_repeater.Layout(layout);
        m_repeater.ItemTemplate(winrt::make<RowFactory>());
        m_repeater.VerticalCacheLength(0.5);
        m_repeater.HorizontalCacheLength(0);
        m_repeater.ItemsSource(winrt::single_threaded_vector<IInspectable>(
            std::vector<IInspectable>(m_listing.entries.size(), winrt::box_value(0))));
        m_preparedRevoker = m_repeater.ElementPrepared(winrt::auto_revoke, [weak](auto const&, ItemsRepeaterElementPreparedEventArgs const& args) {
            if (auto self = weak.lock(); self && self->m_loaded)
            {
                if (auto row = args.Element().try_as<Grid>())
                {
                    self->FillRow(row, static_cast<uint32_t>(args.Index()));
                }
            }
        });

        m_scroll = ScrollViewer{};
        m_scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        m_scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        m_scroll.IsTabStop(false);
        m_scroll.Content(m_repeater);
        Grid::SetRow(m_scroll, 3);
        root.Children().Append(m_scroll);

        winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(root, winrt::hstring{summaryText});
        m_root = root;
        return m_root;
    }

    void ArchivePreviewer::FillRow(Grid const& row, uint32_t index) const
    {
        if (index >= m_listing.entries.size() || row.Children().Size() < 5)
        {
            return;
        }
        auto const& entry = m_listing.entries[index];
        auto children = row.Children();
        children.GetAt(0).as<FontIcon>().Glyph(entry.isDir ? kFolderGlyph : kFileGlyph);
        children.GetAt(1).as<TextBlock>().Text(winrt::hstring{entry.path});
        children.GetAt(2).as<TextBlock>().Text((!entry.isDir && entry.sizeKnown) ? winrt::hstring{FormatBytes(entry.size)} : winrt::hstring{});
        children.GetAt(3).as<TextBlock>().Text((!entry.isDir && entry.compressedSize) ? winrt::hstring{FormatBytes(*entry.compressedSize)}
                                                                                       : winrt::hstring{});
        children.GetAt(4).as<TextBlock>().Text(winrt::hstring{FormatUnixTime(entry.mtime)});
        winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(row, winrt::hstring{entry.path});
    }

    bool ArchivePreviewer::OnKey(winrt::Windows::System::VirtualKey key, bool ctrl, bool shift)
    {
        if (ctrl || shift)
        {
            return false;
        }
        return ScrollKey(key);
    }

    bool ArchivePreviewer::Execute(std::wstring_view command)
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

    bool ArchivePreviewer::ScrollKey(winrt::Windows::System::VirtualKey key)
    {
        using winrt::Windows::System::VirtualKey;
        if (!m_loaded || !m_scroll)
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

    void ArchivePreviewer::Unload()
    {
        m_loaded = false;
        m_preparedRevoker.revoke();
        if (m_repeater)
        {
            m_repeater.ItemsSource(nullptr);
        }
        m_repeater = nullptr;
        m_scroll = nullptr;
        m_root = nullptr;
        m_listing = core::ArchiveListing{};
        m_archiveSize = 0;
    }

    std::shared_ptr<IPreviewer> CreateArchivePreviewer()
    {
        return std::make_shared<ArchivePreviewer>();
    }
}
