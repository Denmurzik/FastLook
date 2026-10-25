#include "pch.h"
#include "Previewers/Markdown/MdRenderer.h"

#include <algorithm>
#include <cwctype>
#include <optional>

#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>

#include "Diagnostics/Log.h"
#include "FileTypes/Extensions.h"
#include "Previewers/Image/WicDecoder.h"
#include "Previewers/PreviewContext.h"
#include "Text/Highlight.h"
#include "Text/Utf.h"
#include "Util/Async.h"
#include "Util/Str.h"

namespace fastlook::viewer
{
    namespace
    {
        namespace fs = std::filesystem;
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        using namespace winrt::Microsoft::UI::Xaml::Documents;
        using winrt::Microsoft::UI::Xaml::Media::SolidColorBrush;
        using winrt::Windows::UI::Color;
        using core::md::Block;
        using core::md::BlockType;
        using core::md::Span;
        using core::md::SpanFlag;

        constexpr size_t kMaxImages = 40;          // pictures decoded per document; the rest show their alt text
        constexpr size_t kMaxHighlightBytes = 128 * 1024;
        constexpr int kMaxDepth = 32;              // XAML nesting guard (the model flattens at 64)
        constexpr size_t kMaxLinkLength = 2048;
        constexpr double kContentWidth = 860.0;    // DIP, the reading column
        constexpr double kSideMargin = 32.0;
        constexpr double kScrollBarAllowance = 16.0;
        constexpr uint32_t kMaxImageHeightPx = 4096;

        constexpr double kHeadingSize[] = {28, 22, 18, 16, 14, 13};

        Color Rgb(uint32_t rgb)
        {
            return {255, static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb)};
        }

        struct Palette
        {
            Color background, text, subtle, link, codeBackground, border, headerBackground;
            Color token[9]; // indexed by core::TokenCategory
        };

        Palette const& PaletteFor(bool dark)
        {
            static Palette const light{Rgb(0xFFFFFF), Rgb(0x1F2328), Rgb(0x59636E), Rgb(0x0969DA), Rgb(0xF6F8FA), Rgb(0xD1D9E0), Rgb(0xEEF1F4),
                                       {Rgb(0x1F1F1F), Rgb(0x008000), Rgb(0xA31515), Rgb(0x098658), Rgb(0x0000FF), Rgb(0xAF00DB), Rgb(0x1F1F1F),
                                        Rgb(0x001080), Rgb(0xE51400)}};
            static Palette const darkPalette{Rgb(0x1E1E1E), Rgb(0xE6EDF3), Rgb(0x9198A1), Rgb(0x4493F8), Rgb(0x252526), Rgb(0x3D444D), Rgb(0x2D2D2D),
                                             {Rgb(0xD4D4D4), Rgb(0x6A9955), Rgb(0xCE9178), Rgb(0xB5CEA8), Rgb(0x569CD6), Rgb(0xC586C0), Rgb(0xD4D4D4),
                                              Rgb(0x9CDCFE), Rgb(0xF44747)}};
            return dark ? darkPalette : light;
        }

        SolidColorBrush Brush(Color color)
        {
            return SolidColorBrush{color};
        }

        // ---- Links and image sources --------------------------------------------------------------------------------------

        std::wstring_view Trim(std::wstring_view s)
        {
            while (!s.empty() && (s.front() == L' ' || s.front() == L'\t'))
            {
                s.remove_prefix(1);
            }
            while (!s.empty() && (s.back() == L' ' || s.back() == L'\t'))
            {
                s.remove_suffix(1);
            }
            return s;
        }

        // The URI scheme (lower case) when `s` starts with one; a single letter is a drive, not a scheme.
        std::wstring SchemeOf(std::wstring_view s)
        {
            size_t const colon = s.find(L':');
            if (colon == std::wstring_view::npos || colon < 2)
            {
                return {};
            }
            std::wstring scheme;
            for (size_t i = 0; i < colon; ++i)
            {
                wchar_t const c = s[i];
                bool const ok = (c < 128 && std::iswalnum(c)) || c == L'+' || c == L'-' || c == L'.';
                if (!ok || (i == 0 && !std::iswalpha(c)))
                {
                    return {};
                }
                scheme.push_back(static_cast<wchar_t>(std::towlower(c)));
            }
            return scheme;
        }

        bool IsOpenableLink(std::wstring_view href)
        {
            href = Trim(href);
            if (href.empty() || href.size() > kMaxLinkLength)
            {
                return false;
            }
            if (std::any_of(href.begin(), href.end(), [](wchar_t c) { return c < 0x20 || c == 0x7F; }))
            {
                return false;
            }
            std::wstring const scheme = SchemeOf(href);
            if (scheme != L"http" && scheme != L"https" && scheme != L"mailto")
            {
                return false;
            }
            return href.size() > scheme.size() + 1;
        }

        void OpenLink(std::wstring const& href)
        {
            if (!IsOpenableLink(href))
            {
                return;
            }
            std::wstring const target{Trim(href)};
            auto const result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (result <= 32)
            {
                FL_LOG_INFO(L"Opening a markdown link failed, code {}", result);
            }
        }

        std::wstring PercentDecode(std::wstring_view s)
        {
            std::string bytes;
            auto hex = [](wchar_t c) -> int {
                if (c >= L'0' && c <= L'9') return c - L'0';
                if (c >= L'a' && c <= L'f') return c - L'a' + 10;
                if (c >= L'A' && c <= L'F') return c - L'A' + 10;
                return -1;
            };
            std::wstring pendingWide;
            auto flushWide = [&] {
                if (!pendingWide.empty())
                {
                    bytes += core::ToUtf8(pendingWide);
                    pendingWide.clear();
                }
            };
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == L'%' && i + 2 < s.size() + 0 && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0)
                {
                    flushWide();
                    bytes.push_back(static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2])));
                    i += 2;
                }
                else
                {
                    pendingWide.push_back(s[i]);
                }
            }
            flushWide();
            return core::FromUtf8(bytes);
        }

        // The file a markdown image source points at, or nothing for sources that are never loaded: http, https, data, any other
        // scheme, protocol-relative (//) and UNC paths (they would reach the network).
        std::optional<fs::path> LocalImagePath(std::wstring_view src, fs::path const& baseDir)
        {
            src = Trim(src);
            if (src.empty() || src.starts_with(L"//") || src.starts_with(L"\\\\"))
            {
                return std::nullopt;
            }
            std::wstring_view rest = src;
            std::wstring const scheme = SchemeOf(src);
            if (!scheme.empty())
            {
                if (scheme != L"file")
                {
                    return std::nullopt;
                }
                rest.remove_prefix(5);
                if (rest.starts_with(L"//"))
                {
                    rest.remove_prefix(2);
                    if (rest.starts_with(L"localhost/"))
                    {
                        rest.remove_prefix(9);
                    }
                    else if (!rest.starts_with(L"/"))
                    {
                        return std::nullopt; // file://host/share
                    }
                }
                // file:///C:/x -> "/C:/x" -> "C:/x"
                if (rest.size() >= 3 && rest[0] == L'/' && std::iswalpha(rest[1]) && rest[2] == L':')
                {
                    rest.remove_prefix(1);
                }
            }
            if (size_t const cut = rest.find_first_of(L"?#"); cut != std::wstring_view::npos)
            {
                rest = rest.substr(0, cut);
            }
            std::wstring decoded = PercentDecode(rest);
            if (decoded.empty() || decoded.find(L'\0') != std::wstring::npos)
            {
                return std::nullopt;
            }
            std::replace(decoded.begin(), decoded.end(), L'/', L'\\');
            if (decoded.starts_with(L"\\\\"))
            {
                return std::nullopt;
            }
            fs::path path{decoded};
            if (path.has_root_directory() && !path.has_root_name())
            {
                path = baseDir.root_name() / path;
            }
            else if (path.is_relative())
            {
                path = baseDir / path;
            }
            path = path.lexically_normal();
            if (path.native().starts_with(L"\\\\"))
            {
                return std::nullopt;
            }
            return path;
        }

        // ---- Inlines ------------------------------------------------------------------------------------------------------

        struct ImageJob
        {
            Image image{nullptr};
            InlineUIContainer holder{nullptr};
            fs::path path;
            std::wstring alt;
        };

        struct RenderState
        {
            Palette const* palette = nullptr;
            fs::path baseDir;
            std::vector<ImageJob> jobs;
            bool subdued = false; // inside a quote
        };

        struct Fmt
        {
            bool bold = false, italic = false, code = false, strike = false;
        };

        winrt::Microsoft::UI::Xaml::Media::FontFamily MonoFont()
        {
            return winrt::Microsoft::UI::Xaml::Media::FontFamily{L"Cascadia Mono, Consolas"};
        }

        Color TextColor(RenderState const& st)
        {
            return st.subdued ? st.palette->subtle : st.palette->text;
        }

        Run MakeRun(std::wstring_view text, Fmt fmt, RenderState const& st, std::optional<Color> color = std::nullopt)
        {
            Run run;
            run.Text(winrt::hstring{text});
            if (fmt.bold)
            {
                run.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
            }
            if (fmt.italic)
            {
                run.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
            }
            if (fmt.strike)
            {
                run.TextDecorations(winrt::Windows::UI::Text::TextDecorations::Strikethrough);
            }
            if (fmt.code)
            {
                run.FontFamily(MonoFont());
                run.FontSize(13);
            }
            if (color)
            {
                run.Foreground(Brush(*color));
            }
            else if (fmt.code)
            {
                run.Foreground(Brush(st.palette->token[static_cast<size_t>(core::TokenCategory::Identifier)]));
            }
            return run;
        }

        // Text with embedded line feeds (hard breaks, code) becomes runs separated by LineBreaks.
        void AddText(InlineCollection const& inlines, std::wstring_view text, Fmt fmt, RenderState const& st,
                     std::optional<Color> color = std::nullopt)
        {
            size_t start = 0;
            while (true)
            {
                size_t end = text.find(L'\n', start);
                std::wstring_view line = text.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start);
                if (!line.empty() && line.back() == L'\r')
                {
                    line.remove_suffix(1);
                }
                if (!line.empty())
                {
                    inlines.Append(MakeRun(line, fmt, st, color));
                }
                if (end == std::wstring_view::npos)
                {
                    break;
                }
                inlines.Append(LineBreak{});
                start = end + 1;
            }
        }

        std::wstring AltText(Span const& span)
        {
            if (!span.text.empty())
            {
                return span.text;
            }
            return std::wstring{Str(L"Markdown", L"ImageAlt")};
        }

        void AppendSpans(InlineCollection const& inlines, std::vector<Span> const& spans, Fmt base, RenderState& st);

        void AppendImage(InlineCollection const& inlines, Span const& span, Fmt base, RenderState& st)
        {
            bool const linked = (span.flags & static_cast<uint8_t>(SpanFlag::Link)) != 0 && IsOpenableLink(span.linkHref);
            std::optional<fs::path> path = LocalImagePath(span.href, st.baseDir);
            if (path && st.jobs.size() < kMaxImages)
            {
                Image image;
                image.Stretch(winrt::Microsoft::UI::Xaml::Media::Stretch::Uniform);
                image.HorizontalAlignment(HorizontalAlignment::Left);
                InlineUIContainer holder;
                holder.Child(image);
                if (linked)
                {
                    std::wstring const target = span.linkHref;
                    image.Tapped([target](auto const&, auto const&) { OpenLink(target); });
                }
                inlines.Append(holder);
                st.jobs.push_back({image, holder, *path, AltText(span)});
                return;
            }
            // Remote or unreadable: the alt text, italic. Nothing is fetched.
            Fmt fmt = base;
            fmt.italic = true;
            std::wstring const alt = AltText(span);
            if (linked)
            {
                Hyperlink link;
                link.Foreground(Brush(st.palette->link));
                AddText(link.Inlines(), alt, fmt, st);
                std::wstring const target = span.linkHref;
                link.Click([target](auto const&, auto const&) { OpenLink(target); });
                inlines.Append(link);
            }
            else
            {
                AddText(inlines, alt, fmt, st, st.palette->subtle);
            }
        }

        void AppendSpans(InlineCollection const& inlines, std::vector<Span> const& spans, Fmt base, RenderState& st)
        {
            for (Span const& span : spans)
            {
                uint8_t const flags = span.flags;
                if (flags & static_cast<uint8_t>(SpanFlag::Image))
                {
                    AppendImage(inlines, span, base, st);
                    continue;
                }
                Fmt fmt = base;
                fmt.bold |= (flags & static_cast<uint8_t>(SpanFlag::Bold)) != 0;
                fmt.italic |= (flags & static_cast<uint8_t>(SpanFlag::Italic)) != 0;
                fmt.strike |= (flags & static_cast<uint8_t>(SpanFlag::Strike)) != 0;
                fmt.code |= (flags & (static_cast<uint8_t>(SpanFlag::Code) | static_cast<uint8_t>(SpanFlag::RawHtml))) != 0;
                if (flags & static_cast<uint8_t>(SpanFlag::Link))
                {
                    Hyperlink link;
                    link.Foreground(Brush(st.palette->link));
                    AddText(link.Inlines(), span.text, fmt, st, st.palette->link);
                    std::wstring const target = span.href;
                    link.Click([target](auto const&, auto const&) { OpenLink(target); });
                    inlines.Append(link);
                }
                else
                {
                    AddText(inlines, span.text, fmt, st);
                }
            }
        }

        // ---- Blocks -------------------------------------------------------------------------------------------------------

        RichTextBlock MakeTextBlock(std::vector<Span> const& spans, double size, Fmt base, RenderState& st)
        {
            RichTextBlock text;
            text.TextWrapping(TextWrapping::Wrap);
            text.IsTextSelectionEnabled(true);
            text.FontSize(size);
            text.Foreground(Brush(TextColor(st)));
            Paragraph paragraph;
            AppendSpans(paragraph.Inlines(), spans, base, st);
            text.Blocks().Append(paragraph);
            return text;
        }

        void BuildBlocks(UIElementCollection const& out, std::vector<Block> const& blocks, RenderState& st, int depth);

        std::wstring Bullet(int depth)
        {
            static constexpr wchar_t kBullets[] = {L'\u2022', L'\u25E6', L'\u25AA'};
            return std::wstring(1, kBullets[depth % 3]);
        }

        UIElement BuildList(Block const& list, RenderState& st, int depth)
        {
            StackPanel panel;
            panel.Spacing(4);
            unsigned number = list.start;
            for (Block const& item : list.children)
            {
                if (item.type != BlockType::ListItem)
                {
                    continue;
                }
                Grid row;
                ColumnDefinition markerColumn;
                markerColumn.Width(winrt::Microsoft::UI::Xaml::GridLength{1, winrt::Microsoft::UI::Xaml::GridUnitType::Auto});
                markerColumn.MinWidth(24);
                ColumnDefinition contentColumn;
                row.ColumnDefinitions().Append(markerColumn);
                row.ColumnDefinitions().Append(contentColumn);

                TextBlock marker;
                marker.FontSize(14);
                marker.Foreground(Brush(TextColor(st)));
                marker.TextAlignment(TextAlignment::Right);
                marker.Margin({0, 0, 8, 0});
                if (item.checked)
                {
                    marker.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily{L"Segoe UI Symbol"});
                    marker.Text(*item.checked ? L"\u2611" : L"\u2610");
                }
                else if (list.ordered)
                {
                    marker.Text(winrt::hstring{std::to_wstring(number) + L"."});
                }
                else
                {
                    marker.Text(winrt::hstring{Bullet(depth)});
                }
                ++number;
                Grid::SetColumn(marker, 0);

                StackPanel content;
                content.Spacing(6);
                Grid::SetColumn(content, 1);
                if (!item.spans.empty())
                {
                    content.Children().Append(MakeTextBlock(item.spans, 14, {}, st));
                }
                BuildBlocks(content.Children(), item.children, st, depth + 1);

                row.Children().Append(marker);
                row.Children().Append(content);
                panel.Children().Append(row);
            }
            return panel;
        }

        UIElement BuildQuote(Block const& quote, RenderState& st, int depth)
        {
            Border border;
            border.BorderThickness({3, 0, 0, 0});
            border.BorderBrush(Brush(st.palette->border));
            border.Padding({14, 2, 0, 2});
            StackPanel panel;
            panel.Spacing(8);
            bool const wasSubdued = st.subdued;
            st.subdued = true;
            BuildBlocks(panel.Children(), quote.children, st, depth + 1);
            st.subdued = wasSubdued;
            border.Child(panel);
            return border;
        }

        size_t SpanLength(std::vector<Span> const& spans)
        {
            size_t n = 0;
            for (Span const& s : spans)
            {
                n += s.text.size();
            }
            return n;
        }

        UIElement BuildTable(Block const& table, RenderState& st)
        {
            std::vector<Block const*> rows;
            size_t columns = 0;
            for (Block const& row : table.children)
            {
                if (row.type == BlockType::TableRow)
                {
                    rows.push_back(&row);
                    columns = std::max(columns, row.children.size());
                }
            }
            if (rows.empty() || columns == 0)
            {
                return nullptr;
            }

            // Column widths follow the text lengths, so short columns stay narrow and long ones wrap.
            std::vector<double> weight(columns, 6.0);
            for (Block const* row : rows)
            {
                for (size_t c = 0; c < row->children.size(); ++c)
                {
                    weight[c] = std::max(weight[c], static_cast<double>(std::min<size_t>(SpanLength(row->children[c].spans), 48)));
                }
            }

            Grid grid;
            for (size_t c = 0; c < columns; ++c)
            {
                ColumnDefinition column;
                column.Width(winrt::Microsoft::UI::Xaml::GridLength{weight[c], winrt::Microsoft::UI::Xaml::GridUnitType::Star});
                grid.ColumnDefinitions().Append(column);
            }
            for (size_t r = 0; r < rows.size(); ++r)
            {
                RowDefinition rowDef;
                rowDef.Height(winrt::Microsoft::UI::Xaml::GridLength{1, winrt::Microsoft::UI::Xaml::GridUnitType::Auto});
                grid.RowDefinitions().Append(rowDef);
                for (size_t c = 0; c < columns; ++c)
                {
                    Border cell;
                    cell.BorderThickness({0, 0, 1, 1});
                    cell.BorderBrush(Brush(st.palette->border));
                    cell.Padding({10, 6, 10, 6});
                    bool header = rows[r]->header;
                    if (c < rows[r]->children.size())
                    {
                        Block const& data = rows[r]->children[c];
                        header = header || data.header;
                        RichTextBlock text = MakeTextBlock(data.spans, 14, Fmt{header, false, false, false}, st);
                        switch (data.align)
                        {
                        case core::md::Align::Center:
                            text.TextAlignment(TextAlignment::Center);
                            break;
                        case core::md::Align::Right:
                            text.TextAlignment(TextAlignment::Right);
                            break;
                        default:
                            text.TextAlignment(TextAlignment::Left);
                            break;
                        }
                        cell.Child(text);
                    }
                    if (header)
                    {
                        cell.Background(Brush(st.palette->headerBackground));
                    }
                    Grid::SetRow(cell, static_cast<int>(r));
                    Grid::SetColumn(cell, static_cast<int>(c));
                    grid.Children().Append(cell);
                }
            }
            Border frame;
            frame.BorderThickness({1, 1, 0, 0});
            frame.BorderBrush(Brush(st.palette->border));
            frame.Child(grid);
            return frame;
        }

        std::wstring_view StripFinalNewline(std::wstring_view text)
        {
            if (text.ends_with(L'\n'))
            {
                text.remove_suffix(1);
                if (text.ends_with(L'\r'))
                {
                    text.remove_suffix(1);
                }
            }
            return text;
        }

        // First word of the info string ("cpp {1,3}" -> "cpp").
        std::wstring LanguageTag(std::wstring_view lang)
        {
            lang = Trim(lang);
            size_t const end = lang.find_first_of(L" \t{");
            if (end != std::wstring_view::npos)
            {
                lang = lang.substr(0, end);
            }
            return std::wstring{lang};
        }

        UIElement BuildCode(Block const& code, RenderState& st)
        {
            std::wstring_view const text = StripFinalNewline(code.text);
            RichTextBlock block;
            block.IsTextSelectionEnabled(true);
            block.TextWrapping(TextWrapping::NoWrap);
            block.FontFamily(MonoFont());
            block.FontSize(13);
            block.Foreground(Brush(st.palette->text));
            Paragraph paragraph;
            InlineCollection inlines = paragraph.Inlines();
            Fmt const plain{};

            std::string_view lexer;
            std::wstring const lang = LanguageTag(code.lang);
            if (!lang.empty())
            {
                lexer = core::LexerNameFor(L"x." + lang);
            }
            std::string const utf8 = core::ToUtf8(text);
            std::vector<core::StyleRun> runs;
            if (!lexer.empty() && !utf8.empty() && utf8.size() <= kMaxHighlightBytes)
            {
                runs = core::HighlightBuffer(utf8, lexer);
            }
            if (runs.empty())
            {
                AddText(inlines, text, plain, st, st.palette->text);
            }
            else
            {
                size_t position = 0;
                auto emit = [&](size_t from, size_t to, core::TokenCategory category) {
                    if (to > from)
                    {
                        AddText(inlines, core::FromUtf8(std::string_view{utf8}.substr(from, to - from)), plain, st,
                                st.palette->token[static_cast<size_t>(category)]);
                    }
                };
                for (core::StyleRun const& run : runs)
                {
                    size_t const start = std::min<size_t>(run.start, utf8.size());
                    size_t const end = std::min<size_t>(static_cast<size_t>(run.start) + run.length, utf8.size());
                    if (start < position)
                    {
                        continue;
                    }
                    emit(position, start, core::TokenCategory::Default);
                    emit(start, end, run.category);
                    position = end;
                }
                emit(position, utf8.size(), core::TokenCategory::Default);
            }
            block.Blocks().Append(paragraph);

            ScrollViewer scroller;
            scroller.HorizontalScrollBarVisibility(ScrollBarVisibility::Auto);
            scroller.HorizontalScrollMode(ScrollMode::Auto);
            scroller.VerticalScrollBarVisibility(ScrollBarVisibility::Disabled);
            scroller.VerticalScrollMode(ScrollMode::Disabled);
            scroller.Content(block);

            Border border;
            border.Background(Brush(st.palette->codeBackground));
            border.BorderBrush(Brush(st.palette->border));
            border.BorderThickness({1, 1, 1, 1});
            border.CornerRadius({4, 4, 4, 4});
            border.Padding({12, 10, 12, 10});
            border.Child(scroller);
            return border;
        }

        UIElement BuildHtml(Block const& html, RenderState& st)
        {
            RichTextBlock block;
            block.IsTextSelectionEnabled(true);
            block.TextWrapping(TextWrapping::Wrap);
            block.FontFamily(MonoFont());
            block.FontSize(13);
            block.Foreground(Brush(st.palette->subtle));
            Paragraph paragraph;
            AddText(paragraph.Inlines(), StripFinalNewline(html.text), {}, st, st.palette->subtle);
            block.Blocks().Append(paragraph);
            return block;
        }

        UIElement BuildHeading(Block const& heading, RenderState& st)
        {
            int const level = std::clamp(heading.level, 1, 6);
            RichTextBlock text = MakeTextBlock(heading.spans, kHeadingSize[level - 1], {}, st);
            text.FontWeight(winrt::Windows::UI::Text::FontWeight{600});
            if (level > 2)
            {
                text.Margin({0, 6, 0, 0});
                return text;
            }
            Border border;
            border.BorderThickness({0, 0, 0, 1});
            border.BorderBrush(Brush(st.palette->border));
            border.Padding({0, 6, 0, 6});
            border.Margin({0, 8, 0, 0});
            border.Child(text);
            return border;
        }

        void BuildBlocks(UIElementCollection const& out, std::vector<Block> const& blocks, RenderState& st, int depth)
        {
            if (depth > kMaxDepth)
            {
                return;
            }
            for (Block const& block : blocks)
            {
                UIElement element{nullptr};
                switch (block.type)
                {
                case BlockType::Heading:
                    element = BuildHeading(block, st);
                    break;
                case BlockType::Paragraph:
                    if (!block.spans.empty())
                    {
                        element = MakeTextBlock(block.spans, 14, {}, st);
                    }
                    break;
                case BlockType::List:
                    element = BuildList(block, st, depth);
                    break;
                case BlockType::Quote:
                    element = BuildQuote(block, st, depth);
                    break;
                case BlockType::Table:
                    element = BuildTable(block, st);
                    break;
                case BlockType::Code:
                    element = BuildCode(block, st);
                    break;
                case BlockType::Html:
                    element = BuildHtml(block, st);
                    break;
                case BlockType::HRule:
                {
                    Border line;
                    line.Height(1);
                    line.Margin({0, 8, 0, 8});
                    line.Background(Brush(st.palette->border));
                    element = line;
                    break;
                }
                default:
                    break; // ListItem / TableRow / TableCell are handled by their parents
                }
                if (element)
                {
                    out.Append(element);
                }
            }
        }

        // ---- Pictures -----------------------------------------------------------------------------------------------------

        struct ImageLoad
        {
            std::vector<ImageJob> jobs;
            std::shared_ptr<MdImages> images;
            std::stop_token stop;
            winrt::com_ptr<IWICImagingFactory2> wic;
            winrt::Microsoft::UI::Dispatching::DispatcherQueue ui{nullptr};
            double scale = 1.0;
            Color altColor{};
        };

        void ShowAlt(ImageJob const& job, Color color)
        {
            TextBlock text;
            text.Text(winrt::hstring{job.alt});
            text.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
            text.Foreground(Brush(color));
            job.holder.Child(text);
        }

        // One picture at a time keeps the transient memory at a single decode.
        winrt::fire_and_forget LoadImages(std::shared_ptr<ImageLoad> load)
        {
            try
            {
                core::SizeU const target{static_cast<uint32_t>(std::ceil(kContentWidth * load->scale)), kMaxImageHeightPx};
                for (ImageJob const& job : load->jobs)
                {
                    if (load->stop.stop_requested() || load->images->closed)
                    {
                        co_return;
                    }
                    DecodedImage decoded;
                    bool failed = false;
                    co_await winrt::resume_background();
                    try
                    {
                        decoded = DecodeToSize(job.path, target, load->stop, load->wic);
                    }
                    catch (PreviewError const&)
                    {
                        failed = true;
                    }
                    catch (winrt::hresult_error const&)
                    {
                        failed = true;
                    }
                    if (!co_await ResumeOn(load->ui))
                    {
                        co_return;
                    }
                    if (load->stop.stop_requested() || load->images->closed)
                    {
                        if (decoded.bitmap)
                        {
                            decoded.bitmap.Close();
                        }
                        co_return;
                    }
                    if (failed || !decoded.bitmap)
                    {
                        ShowAlt(job, load->altColor);
                        continue;
                    }
                    double const width = decoded.bitmap.PixelWidth() / load->scale;
                    winrt::Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource source;
                    co_await source.SetBitmapAsync(decoded.bitmap);
                    decoded.bitmap.Close();
                    if (load->stop.stop_requested() || load->images->closed)
                    {
                        source.Close();
                        co_return;
                    }
                    load->images->sources.push_back(source);
                    job.image.MaxWidth(width);
                    job.image.Source(source);
                }
            }
            catch (...)
            {
                FL_LOG_INFO(L"Loading markdown images failed");
            }
        }
    }

    void MdImages::Close()
    {
        closed = true;
        for (auto& source : sources)
        {
            try
            {
                source.Close();
            }
            catch (...)
            {
            }
        }
        sources.clear();
    }

    UIElement RenderMarkdown(core::md::Document const& document, std::filesystem::path const& baseDir, bool dark, std::stop_token stop,
                             std::shared_ptr<MdImages>* images)
    {
        Palette const& palette = PaletteFor(dark);
        RenderState state;
        state.palette = &palette;
        state.baseDir = baseDir;

        StackPanel content;
        content.Spacing(12);
        content.HorizontalAlignment(HorizontalAlignment::Center);
        content.Width(kContentWidth);
        content.Margin({kSideMargin, 24, kSideMargin, 32});
        BuildBlocks(content.Children(), document.blocks, state, 0);

        ScrollViewer scroller;
        scroller.Background(Brush(palette.background));
        scroller.RequestedTheme(dark ? ElementTheme::Dark : ElementTheme::Light);
        scroller.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroller.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        scroller.IsTabStop(false);
        scroller.Content(content);
        // The reading column is the viewport minus its margins, at most kContentWidth. A ScrollViewer with horizontal scrolling
        // disabled still hands its content the unconstrained desired width of wide children (tables), so the width is set here.
        scroller.SizeChanged([content](auto const& sender, auto const&) {
            double const available = sender.as<ScrollViewer>().ActualWidth() - 2 * kSideMargin - kScrollBarAllowance;
            content.Width(std::clamp(available, 120.0, kContentWidth));
        });

        auto holder = std::make_shared<MdImages>();
        if (images)
        {
            *images = holder;
        }
        if (!state.jobs.empty())
        {
            auto load = std::make_shared<ImageLoad>();
            load->jobs = std::move(state.jobs);
            load->images = holder;
            load->stop = stop;
            load->wic = WicFactory(); // taken here, on the UI thread, for the background decodes
            load->ui = scroller.DispatcherQueue();
            load->scale = std::max(1.0, GetDpiForSystem() / 96.0);
            load->altColor = palette.subtle;
            if (load->wic)
            {
                LoadImages(std::move(load));
            }
            else
            {
                for (ImageJob const& job : load->jobs)
                {
                    ShowAlt(job, palette.subtle);
                }
            }
        }
        return scroller;
    }
}
