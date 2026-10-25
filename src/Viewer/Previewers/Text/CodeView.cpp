#include "pch.h"
#include "Previewers/Text/CodeView.h"

#include <algorithm>
#include <cmath>

#include <winrt/Windows.UI.h>

#include <chrono>

#include "Diagnostics/Log.h"
#include "Diagnostics/PerfLog.h"
#include "Graphics/DeviceResources.h"
#include "Util/Str.h"

namespace fastlook::viewer
{
    namespace
    {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        using core::TokenCategory;

        constexpr size_t kCategoryCount = static_cast<size_t>(TokenCategory::Error) + 1;
        constexpr float kMinFontSize = 6, kMaxFontSize = 72;
        constexpr double kGutterPadLeft = 10, kGutterPadRight = 12, kTextPadLeft = 10, kEndPad = 24;
        constexpr double kWheelLinesPerNotch = 3, kWheelHorizontalDipPerNotch = 60;
        // Lines longer than this are laid out only for the horizontally visible columns (a 16 384-unit segment per row would
        // otherwise be shaped on every redraw).
        constexpr size_t kWindowedLineUnits = 1000;
        constexpr size_t kWindowSlackColumns = 64;
        constexpr double kWrapPadRight = 12;
        constexpr size_t kMaxCopyUnits = 16u * 1024 * 1024;
        constexpr int64_t kNoticeMs = 4000;
        constexpr size_t kLineCountCacheMax = 1024;
        constexpr int64_t kDragIntervalMs = 50;
        constexpr double kMaxDragRowsPerTick = 20;

        D2D1_COLOR_F Rgb(uint32_t rgb)
        {
            return D2D1::ColorF(rgb);
        }

        uint32_t DigitsOf(uint64_t value)
        {
            uint32_t digits = 1;
            while (value >= 10)
            {
                value /= 10;
                ++digits;
            }
            return digits;
        }

        bool IsDeviceLoss(HRESULT hr)
        {
            return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == D2DERR_RECREATE_TARGET;
        }

        bool IsWordUnit(wchar_t c)
        {
            if (c == L'_')
            {
                return true;
            }
            WORD type = 0;
            return GetStringTypeW(CT_CTYPE1, &c, 1, &type) && (type & (C1_ALPHA | C1_DIGIT)) != 0;
        }

        bool FamilyExists(IDWriteFactory3* factory, wchar_t const* family)
        {
            winrt::com_ptr<IDWriteFontCollection> collection;
            if (FAILED(factory->GetSystemFontCollection(collection.put(), FALSE)))
            {
                return false;
            }
            UINT32 index = 0;
            BOOL exists = FALSE;
            return SUCCEEDED(collection->FindFamilyName(family, &index, &exists)) && exists;
        }
    }

    struct CodeView::Palette
    {
        D2D1_COLOR_F background, text, gutterBackground, gutterText, separator, selection;
        D2D1_COLOR_F token[kCategoryCount]; // indexed by TokenCategory
    };

    struct CodeView::Frame
    {
        ID2D1DeviceContext* dc = nullptr;
        winrt::com_ptr<ID2D1SolidColorBrush> text, gutterBackground, gutterText, separator, selection;
        winrt::com_ptr<ID2D1SolidColorBrush> token[kCategoryCount];
        double gutterWidth = 0;
    };

    namespace
    {
        winrt::Windows::UI::Color ToXaml(D2D1_COLOR_F c)
        {
            auto byte = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
            return {255, byte(c.r), byte(c.g), byte(c.b)};
        }
    }

    // VS Code "Light+" / "Dark+" inspired.
    CodeView::Palette const& CodeView::PaletteFor(bool dark)
    {
        {
            static CodeView::Palette const light{
                Rgb(0xFFFFFF), Rgb(0x1F1F1F), Rgb(0xF3F3F3), Rgb(0x8A8A8A), Rgb(0xE0E0E0), Rgb(0xADD6FF),
                {Rgb(0x1F1F1F), Rgb(0x008000), Rgb(0xA31515), Rgb(0x098658), Rgb(0x0000FF), Rgb(0xAF00DB), Rgb(0x1F1F1F), Rgb(0x001080), Rgb(0xE51400)}};
            static CodeView::Palette const darkPalette{
                Rgb(0x1E1E1E), Rgb(0xD4D4D4), Rgb(0x252526), Rgb(0x858585), Rgb(0x333333), Rgb(0x264F78),
                {Rgb(0xD4D4D4), Rgb(0x6A9955), Rgb(0xCE9178), Rgb(0xB5CEA8), Rgb(0x569CD6), Rgb(0xC586C0), Rgb(0xD4D4D4), Rgb(0x9CDCFE), Rgb(0xF44747)}};
            return dark ? darkPalette : light;
        }
    }

    CodeView::CodeView(std::shared_ptr<core::TextDocument> document) : m_doc(std::move(document))
    {
        m_gate = std::make_shared<RedrawGate>();
        m_gate->view = this;
        BuildUi();
        m_gate->ui = m_root.DispatcherQueue();
        m_dwrite.copy_from(DeviceResources::Get().DWrite());
        m_lineCount = m_doc ? m_doc->LineCount() : 0;
        m_gutterDigits = std::max<uint32_t>(3, DigitsOf(m_lineCount));
        RebuildFormat();
    }

    CodeView::~CodeView()
    {
        {
            std::lock_guard lock{m_gate->mutex};
            m_gate->view = nullptr;
        }
        m_hostSize.revoke();
        m_themeChanged.revoke();
        m_contentsLost.revoke();
        m_loaded.revoke();
        m_wheel.revoke();
        m_rootChanged.revoke();
        m_vChanged.revoke();
        m_hChanged.revoke();
        m_pressed.revoke();
        m_moved.revoke();
        m_released.revoke();
        m_captureLost.revoke();
        m_doubleTapped.revoke();
        m_dragTick.revoke();
        m_noticeTick.revoke();
        if (m_noticeTimer)
        {
            m_noticeTimer.Stop();
        }
        if (m_dragTimer)
        {
            m_dragTimer.Stop();
        }
        if (m_image)
        {
            m_image.Source(nullptr);
        }
        m_native = nullptr;
        m_surface = nullptr;
        m_d3d = nullptr;
    }

    UIElement CodeView::Root()
    {
        return m_root;
    }

    void CodeView::BuildUi()
    {
        auto const background = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{ToXaml(PaletteFor(m_dark).background)};

        m_image = Image{};
        m_image.Stretch(winrt::Microsoft::UI::Xaml::Media::Stretch::Fill);
        m_image.HorizontalAlignment(HorizontalAlignment::Left);
        m_image.VerticalAlignment(VerticalAlignment::Top);
        m_image.IsHitTestVisible(false);

        m_host = Grid{};
        m_host.Background(background);
        m_host.Children().Append(m_image);

        m_vbar = winrt::Microsoft::UI::Xaml::Controls::Primitives::ScrollBar{};
        m_vbar.Orientation(Orientation::Vertical);
        m_vbar.IsTabStop(false);
        m_vbar.SmallChange(1);
        m_vbar.IndicatorMode(winrt::Microsoft::UI::Xaml::Controls::Primitives::ScrollingIndicatorMode::MouseIndicator);
        Grid::SetColumn(m_vbar, 1);

        m_hbar = winrt::Microsoft::UI::Xaml::Controls::Primitives::ScrollBar{};
        m_hbar.Orientation(Orientation::Horizontal);
        m_hbar.IsTabStop(false);
        m_hbar.IndicatorMode(winrt::Microsoft::UI::Xaml::Controls::Primitives::ScrollingIndicatorMode::MouseIndicator);
        Grid::SetRow(m_hbar, 1);

        m_root = Grid{};
        m_root.Background(background);
        ColumnDefinition stretchColumn, barColumn;
        barColumn.Width(GridLengthHelper::Auto());
        m_root.ColumnDefinitions().Append(stretchColumn);
        m_root.ColumnDefinitions().Append(barColumn);
        RowDefinition stretchRow, barRow;
        barRow.Height(GridLengthHelper::Auto());
        m_root.RowDefinitions().Append(stretchRow);
        m_root.RowDefinitions().Append(barRow);
        m_root.Children().Append(m_host);
        m_root.Children().Append(m_vbar);
        m_root.Children().Append(m_hbar);

        m_hostSize = m_host.SizeChanged(winrt::auto_revoke, [this](auto const&, auto const&) { OnHostSizeChanged(); });
        m_loaded = m_root.Loaded(winrt::auto_revoke, [this](auto const&, auto const&) {
            if (auto const xamlRoot = m_root.XamlRoot())
            {
                m_rootChanged = xamlRoot.Changed(winrt::auto_revoke, [this](auto const&, auto const&) { OnHostSizeChanged(); });
            }
            OnHostSizeChanged();
        });
        m_wheel = m_root.PointerWheelChanged(winrt::auto_revoke, [this](auto const&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e) {
            OnWheel(e);
        });
        m_pressed = m_host.PointerPressed(winrt::auto_revoke, [this](auto const&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e) {
            OnPointerPressed(e);
        });
        m_moved = m_host.PointerMoved(winrt::auto_revoke, [this](auto const&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e) {
            OnPointerMoved(e);
        });
        m_released = m_host.PointerReleased(winrt::auto_revoke, [this](auto const&, auto const&) { EndDrag(); });
        m_captureLost = m_host.PointerCaptureLost(winrt::auto_revoke, [this](auto const&, auto const&) { EndDrag(); });
        m_doubleTapped = m_host.DoubleTapped(winrt::auto_revoke, [this](auto const&, winrt::Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const& e) {
            auto const point = e.GetPosition(m_host);
            SelectWordAt(point.X, point.Y);
            e.Handled(true);
        });
        // Notice overlay (copy truncated).
        m_noticeText = TextBlock{};
        m_noticeText.TextWrapping(TextWrapping::Wrap);
        m_notice = Border{};
        m_notice.Child(m_noticeText);
        m_notice.Padding({12, 6, 12, 6});
        m_notice.Margin({0, 0, 12, 12});
        m_notice.CornerRadius({4, 4, 4, 4});
        m_notice.HorizontalAlignment(HorizontalAlignment::Right);
        m_notice.VerticalAlignment(VerticalAlignment::Bottom);
        m_notice.Visibility(Visibility::Collapsed);
        m_notice.IsHitTestVisible(false);
        m_noticeText.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{ToXaml(PaletteFor(m_dark).text)});
        m_notice.Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{ToXaml(PaletteFor(m_dark).selection)});
        m_host.Children().Append(m_notice);
        m_noticeTimer = m_root.DispatcherQueue().CreateTimer();
        m_noticeTimer.Interval(std::chrono::milliseconds(kNoticeMs));
        m_noticeTimer.IsRepeating(false);
        m_noticeTick = m_noticeTimer.Tick(winrt::auto_revoke, [this](auto const&, auto const&) { m_notice.Visibility(Visibility::Collapsed); });
        m_dragTimer = m_root.DispatcherQueue().CreateTimer();
        m_dragTimer.Interval(std::chrono::milliseconds(kDragIntervalMs));
        m_dragTimer.IsRepeating(true);
        m_dragTick = m_dragTimer.Tick(winrt::auto_revoke, [this](auto const&, auto const&) { OnDragTick(); });
        m_themeChanged = m_root.ActualThemeChanged(winrt::auto_revoke, [this](auto const&, auto const&) {
            SetDark(m_root.ActualTheme() == ElementTheme::Dark);
        });
        // The compositor lost the surface contents (device reset, sleep/resume, RDP): our own device is usually fine, so no draw
        // error would ever tell us.
        m_contentsLost = winrt::Microsoft::UI::Xaml::Media::CompositionTarget::SurfaceContentsLost(winrt::auto_revoke, [this](auto const&, auto const&) {
            m_d3d = nullptr;
            if (m_surface && RecreateSurface())
            {
                Invalidate();
            }
        });
        m_vChanged = m_vbar.ValueChanged(winrt::auto_revoke, [this](auto const&, auto const& e) {
            if (!m_updatingBars)
            {
                ScrollTo(static_cast<uint64_t>(std::max(0.0, std::round(e.NewValue()))));
            }
        });
        m_hChanged = m_hbar.ValueChanged(winrt::auto_revoke, [this](auto const&, auto const& e) {
            if (!m_updatingBars)
            {
                m_hOffset = std::clamp(e.NewValue(), 0.0, HorizontalMax());
                Invalidate();
            }
        });
    }

    // ---- public state ----------------------------------------------------------------------------------------------

    void CodeView::SetStyler(std::shared_ptr<ILineStyler> styler)
    {
        m_styler = std::move(styler);
        if (m_styler)
        {
            m_styler->invalidated = [gate = m_gate] { CodeView::Invalidate(gate); };
        }
        Invalidate();
    }

    void CodeView::SetWrap(bool wrap)
    {
        if (wrap == m_wrap)
        {
            return;
        }
        // The top row stays in view; the visual-line offset and the horizontal scroll do not carry over.
        m_wrap = wrap;
        m_topSub = 0;
        m_hOffset = 0;
        m_lineCounts.clear();
        m_endValid = false;
        ApplyWrapChrome();
        UpdateBars();
        Invalidate();
    }

    void CodeView::ApplyWrapChrome()
    {
        if (m_hbar)
        {
            m_hbar.Visibility(m_wrap ? Visibility::Collapsed : Visibility::Visible);
        }
    }

    void CodeView::SetFontSize(float dip)
    {
        m_baseFontSize = std::clamp(dip, kMinFontSize, kMaxFontSize);
        SetZoomedSize(m_baseFontSize);
    }

    void CodeView::SetZoomedSize(float dip)
    {
        float const size = std::clamp(dip, kMinFontSize, kMaxFontSize);
        if (size == m_fontSize)
        {
            return;
        }
        m_fontSize = size;
        m_maxLineW = 0;
        m_topSub = 0;
        RebuildFormat();
        UpdateBars();
        Invalidate();
    }

    void CodeView::ZoomSteps(int steps)
    {
        SetZoomedSize(m_fontSize + static_cast<float>(steps));
    }

    void CodeView::ZoomBy(float factor)
    {
        SetZoomedSize(m_fontSize * factor);
    }

    void CodeView::ResetZoom()
    {
        SetZoomedSize(m_baseFontSize);
    }

    void CodeView::SetDark(bool dark)
    {
        if (m_dark == dark)
        {
            return;
        }
        m_dark = dark;
        auto const background = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{ToXaml(PaletteFor(m_dark).background)};
        m_root.Background(background);
        m_host.Background(background);
        m_noticeText.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{ToXaml(PaletteFor(m_dark).text)});
        m_notice.Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{ToXaml(PaletteFor(m_dark).selection)});
        Invalidate();
    }

    bool CodeView::HasSelection() const
    {
        return m_anchor != m_caret;
    }

    // Capped at kMaxCopyUnits (`truncated` says so); a faulted file yields nothing.
    std::wstring CodeView::SelectedText(bool* truncated) const
    {
        bool cut = false;
        std::wstring text;
        if (m_doc && HasSelection())
        {
            text = m_doc->ExtractRangeCapped(std::min(m_anchor, m_caret), std::max(m_anchor, m_caret), kMaxCopyUnits, cut);
        }
        if (truncated)
        {
            *truncated = cut;
        }
        return text;
    }

    // A short line in the bottom-right corner that fades out by itself.
    void CodeView::ShowNotice(std::wstring const& text)
    {
        if (!m_notice || !m_noticeTimer)
        {
            return;
        }
        m_noticeText.Text(winrt::hstring{text});
        m_notice.Visibility(Visibility::Visible);
        m_noticeTimer.Stop();
        m_noticeTimer.Start();
    }

    void CodeView::SelectAll()
    {
        // The document's own count: the progress notifications reach the view a little later than the index.
        uint64_t const lines = m_doc ? m_doc->LineCount() : 0;
        if (lines == 0)
        {
            return;
        }
        uint64_t const last = lines - 1;
        std::wstring text;
        bool continuation = false;
        if (!m_doc->LineText(last, text, continuation))
        {
            return;
        }
        m_anchor = {0, 0};
        m_caret = {last, static_cast<uint32_t>(text.size())};
        Invalidate();
    }

    bool CodeView::OnScrollKey(winrt::Windows::System::VirtualKey key, bool ctrl, bool shift)
    {
        using winrt::Windows::System::VirtualKey;
        if (shift)
        {
            return false;
        }
        int64_t const page = static_cast<int64_t>(std::max<uint64_t>(1, VisibleRows() - 1));
        switch (key)
        {
        case VirtualKey::PageUp:
            if (ctrl)
            {
                return false;
            }
            ScrollLines(-page);
            return true;
        case VirtualKey::PageDown:
            if (ctrl)
            {
                return false;
            }
            ScrollLines(page);
            return true;
        case VirtualKey::Home:
            m_hOffset = 0;
            ScrollTo(0);
            UpdateBars();
            Invalidate();
            return true;
        case VirtualKey::End:
        {
            ScrollPos const end = EndPos();
            SetTop(end.row, end.sub, true);
            return true;
        }
        default:
            return false;
        }
    }

    void CodeView::OnIndexProgress(uint64_t lines, bool complete)
    {
        uint64_t const previous = m_lineCount;
        double const gutterBefore = GutterWidth();
        m_lineCount = std::max(m_lineCount, lines);
        if (complete && m_doc)
        {
            m_lineCount = std::max(m_lineCount, m_doc->LineCount());
        }
        if (m_lineCount == previous)
        {
            return;
        }
        bool const touchesScreen = m_followEnd || previous < m_top + VisibleRows() + 1;
        if (touchesScreen)
        {
            m_cacheValid = false;
        }
        if (m_followEnd)
        {
            ScrollPos const end = EndPos();
            m_top = end.row;
            m_topSub = end.sub;
        }
        UpdateBars();
        if (touchesScreen || GutterWidth() != gutterBefore)
        {
            Invalidate();
        }
    }

    // ---- layout / metrics ------------------------------------------------------------------------------------------

    void CodeView::RebuildFormat()
    {
        m_format = nullptr;
        m_lineCounts.clear();
        m_endValid = false;
        if (!m_dwrite)
        {
            return;
        }
        wchar_t const* family = FamilyExists(m_dwrite.get(), L"Cascadia Mono") ? L"Cascadia Mono"
                                : FamilyExists(m_dwrite.get(), L"Consolas")    ? L"Consolas"
                                                                               : L"Courier New";
        winrt::com_ptr<IDWriteTextFormat> format;
        if (FAILED(m_dwrite->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              m_fontSize, L"en-us", format.put())))
        {
            FL_LOG_INFO(L"CodeView: creating the text format failed");
            return;
        }
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        // Natural metrics of one cell, then snap the line height to whole physical pixels and make it uniform so fallback
        // fonts cannot shift the baseline.
        winrt::com_ptr<IDWriteTextLayout> probe;
        if (FAILED(m_dwrite->CreateTextLayout(L"0", 1, format.get(), 1000.0f, 1000.0f, probe.put())))
        {
            return;
        }
        DWRITE_TEXT_METRICS textMetrics{};
        DWRITE_LINE_METRICS lineMetrics{};
        UINT32 lineCount = 0;
        probe->GetMetrics(&textMetrics);
        probe->GetLineMetrics(&lineMetrics, 1, &lineCount);
        double const scale = m_scale > 0 ? m_scale : 1.0;
        m_charW = std::max(1.0, static_cast<double>(textMetrics.widthIncludingTrailingWhitespace));
        m_lineH = std::ceil(std::max(1.0f, lineMetrics.height) * scale) / scale;
        format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, static_cast<float>(m_lineH),
                               lineMetrics.baseline + static_cast<float>(m_lineH - lineMetrics.height) / 2.0f);
        format->SetIncrementalTabStop(static_cast<float>(m_charW * 4));
        m_format = std::move(format);
    }

    uint64_t CodeView::VisibleRows() const
    {
        return std::max<uint64_t>(1, static_cast<uint64_t>(std::floor(m_viewH / m_lineH)));
    }

    uint64_t CodeView::MaxTop() const
    {
        uint64_t const rows = VisibleRows();
        return m_lineCount > rows ? m_lineCount - rows : 0;
    }

    double CodeView::GutterWidth()
    {
        m_gutterDigits = std::max({m_gutterDigits, 3u, DigitsOf(m_lineCount)});
        return kGutterPadLeft + m_gutterDigits * m_charW + kGutterPadRight;
    }

    double CodeView::HorizontalMax()
    {
        if (m_wrap)
        {
            return 0;
        }
        double const textWidth = m_viewW - GutterWidth() - kTextPadLeft;
        return std::max(0.0, m_maxLineW + kEndPad - textWidth);
    }

    double CodeView::TextLeft()
    {
        return GutterWidth() + kTextPadLeft;
    }

    double CodeView::WrapWidth()
    {
        double const width = std::max(m_charW * 8, m_viewW - TextLeft() - kWrapPadRight);
        if (width != m_wrapWidth)
        {
            m_wrapWidth = width;
            m_lineCounts.clear();
            m_endValid = false;
        }
        return width;
    }

    // Lays one row out. Unwrapped: one visual line, long rows only for the visible columns. Wrapped: the whole row, broken at
    // word boundaries (or anywhere inside a token wider than the view).
    bool CodeView::BuildLayout(std::wstring const& text, RowLayout& out, double wrapWidth)
    {
        out = {};
        if (text.empty() || !m_dwrite || !m_format)
        {
            return false;
        }
        std::wstring_view shown = text;
        if (!m_wrap && text.size() > kWindowedLineUnits)
        {
            size_t start = static_cast<size_t>(std::max(0.0, std::floor(m_hOffset / m_charW) - kWindowSlackColumns));
            start = std::min(start, text.size());
            if (start > 0 && start < text.size() && IS_LOW_SURROGATE(text[start]))
            {
                --start;
            }
            size_t const columns = static_cast<size_t>(std::ceil(std::max(1.0, m_viewW - TextLeft()) / m_charW)) + 2 * kWindowSlackColumns;
            shown = std::wstring_view{text}.substr(start, columns);
            out.base = start;
            out.width = static_cast<double>(text.size()) * m_charW;
        }
        out.shown = shown.size();
        float const maxWidth = m_wrap ? static_cast<float>(wrapWidth) : 1.0e6f;
        if (FAILED(m_dwrite->CreateTextLayout(shown.data(), static_cast<UINT32>(shown.size()), m_format.get(), maxWidth, static_cast<float>(m_lineH),
                                              out.layout.put())))
        {
            out = {};
            return false;
        }
        DWRITE_TEXT_METRICS metrics{};
        if (m_wrap)
        {
            out.layout->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
            out.layout->GetMetrics(&metrics);
            out.lines = std::max<uint32_t>(1, metrics.lineCount);
        }
        else if (out.width == 0)
        {
            out.layout->GetMetrics(&metrics);
            out.width = metrics.widthIncludingTrailingWhitespace;
        }
        return true;
    }

    uint32_t CodeView::VisualLines(uint64_t row)
    {
        if (!m_wrap)
        {
            return 1;
        }
        double const width = WrapWidth();
        if (auto const it = m_lineCounts.find(row); it != m_lineCounts.end())
        {
            return it->second;
        }
        std::wstring text;
        bool continuation = false;
        if (!m_doc || !m_doc->LineText(row, text, continuation))
        {
            return 1;
        }
        RowLayout layout;
        uint32_t const lines = BuildLayout(text, layout, width) ? layout.lines : 1;
        if (m_lineCounts.size() >= kLineCountCacheMax)
        {
            m_lineCounts.clear();
        }
        m_lineCounts[row] = lines;
        return lines;
    }

    CodeView::ScrollPos CodeView::EndPos()
    {
        if (!m_wrap || m_lineCount == 0 || m_viewW <= 0 || m_viewH <= 0)
        {
            return {MaxTop(), 0};
        }
        uint64_t const fit = VisibleRows();
        double const width = WrapWidth();
        if (m_endValid && m_endLineCount == m_lineCount && m_endRows == fit && m_endWidth == width)
        {
            return m_end;
        }
        // Walk back from the last row until the viewport is full.
        uint64_t remaining = fit;
        uint64_t row = m_lineCount - 1;
        ScrollPos end{0, 0};
        for (;;)
        {
            uint64_t const lines = VisualLines(row);
            if (lines >= remaining)
            {
                end = {row, static_cast<uint32_t>(lines - remaining)};
                break;
            }
            remaining -= lines;
            if (row == 0)
            {
                break;
            }
            --row;
        }
        m_end = end;
        m_endValid = true;
        m_endLineCount = m_lineCount;
        m_endRows = fit;
        m_endWidth = width;
        return end;
    }

    // The visual-line offset can fall off its row after a resize or zoom.
    void CodeView::NormalizeTop()
    {
        if (m_wrap && m_topSub > 0 && m_top < m_lineCount)
        {
            m_topSub = std::min(m_topSub, VisualLines(m_top) - 1);
        }
        else if (!m_wrap)
        {
            m_topSub = 0;
        }
    }

    void CodeView::UpdateBars()
    {
        if (!m_vbar)
        {
            return;
        }
        uint64_t const rows = VisibleRows();
        ScrollPos const end = EndPos();
        uint64_t const maxTop = end.row;
        if (ScrollPos{m_top, m_topSub} > end)
        {
            m_top = end.row;
            m_topSub = end.sub;
        }
        NormalizeTop();
        double const maxH = HorizontalMax();
        m_hOffset = std::clamp(m_hOffset, 0.0, maxH);
        double const textViewport = std::max(1.0, m_viewW - GutterWidth());

        m_updatingBars = true;
        m_vbar.Minimum(0);
        m_vbar.Maximum(static_cast<double>(maxTop));
        m_vbar.ViewportSize(static_cast<double>(rows));
        m_vbar.LargeChange(static_cast<double>(std::max<uint64_t>(1, rows - 1)));
        m_vbar.Value(static_cast<double>(m_top));
        m_vbar.IsEnabled(maxTop > 0);

        m_hbar.Minimum(0);
        m_hbar.Maximum(maxH);
        m_hbar.ViewportSize(textViewport);
        m_hbar.LargeChange(textViewport * 0.9);
        m_hbar.SmallChange(m_charW * 4);
        m_hbar.Value(m_hOffset);
        m_hbar.IsEnabled(maxH > 0);
        m_updatingBars = false;
    }

    // ---- scrolling -------------------------------------------------------------------------------------------------

    void CodeView::ScrollLines(int64_t delta)
    {
        if (m_wrap)
        {
            ScrollWrapped(delta);
            return;
        }
        constexpr uint64_t kInt64Max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        int64_t const maxTop = static_cast<int64_t>(std::min(MaxTop(), kInt64Max));
        int64_t const target = std::clamp(static_cast<int64_t>(std::min(m_top, kInt64Max)) + delta, int64_t{0}, maxTop);
        ScrollTo(static_cast<uint64_t>(target));
    }

    void CodeView::ScrollTo(uint64_t top, bool followEnd)
    {
        SetTop(top, 0, followEnd);
    }

    void CodeView::SetTop(uint64_t row, uint32_t sub, bool followEnd)
    {
        m_followEnd = followEnd;
        ScrollPos target{row, sub};
        ScrollPos const end = EndPos();
        if (target > end)
        {
            target = end;
        }
        if (target.row == m_top && target.sub == m_topSub)
        {
            return;
        }
        m_top = target.row;
        m_topSub = target.sub;
        UpdateBars();
        Invalidate();
    }

    // Moves by visual lines through rows of different heights.
    void CodeView::ScrollWrapped(int64_t visualLines)
    {
        if (m_lineCount == 0 || visualLines == 0)
        {
            return;
        }
        ScrollPos const end = EndPos();
        uint64_t row = std::min(m_top, m_lineCount - 1);
        int64_t sub = m_topSub;
        if (visualLines > 0)
        {
            sub += visualLines;
            for (;;)
            {
                if (ScrollPos{row, static_cast<uint32_t>(std::min<int64_t>(sub, std::numeric_limits<uint32_t>::max()))} >= end)
                {
                    SetTop(end.row, end.sub);
                    return;
                }
                int64_t const lines = VisualLines(row);
                if (sub < lines)
                {
                    break;
                }
                sub -= lines;
                ++row;
            }
        }
        else
        {
            sub += visualLines;
            while (sub < 0)
            {
                if (row == 0)
                {
                    sub = 0;
                    break;
                }
                --row;
                sub += VisualLines(row);
            }
        }
        SetTop(row, static_cast<uint32_t>(sub));
    }

    void CodeView::ScrollHorizontal(double deltaDip)
    {
        double const target = std::clamp(m_hOffset + deltaDip, 0.0, HorizontalMax());
        if (target == m_hOffset)
        {
            return;
        }
        m_hOffset = target;
        UpdateBars();
        Invalidate();
    }

    void CodeView::OnWheel(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e)
    {
        auto const properties = e.GetCurrentPoint(m_root).Properties();
        double const notches = properties.MouseWheelDelta() / 120.0;
        if (notches == 0)
        {
            return;
        }
        using winrt::Windows::System::VirtualKeyModifiers;
        auto const modifiers = e.KeyModifiers();
        bool const ctrl = (modifiers & VirtualKeyModifiers::Control) != VirtualKeyModifiers::None;
        bool const shift = (modifiers & VirtualKeyModifiers::Shift) != VirtualKeyModifiers::None;
        if (ctrl)
        {
            // Touchpads send many small deltas: a zoom step per whole notch.
            m_zoomRemainder += notches;
            double whole = 0;
            m_zoomRemainder = std::modf(m_zoomRemainder, &whole);
            if (whole != 0)
            {
                ZoomSteps(static_cast<int>(whole));
            }
        }
        else if (properties.IsHorizontalMouseWheel())
        {
            ScrollHorizontal(notches * kWheelHorizontalDipPerNotch);
        }
        else if (shift)
        {
            ScrollHorizontal(-notches * kWheelHorizontalDipPerNotch);
        }
        else
        {
            m_wheelRemainder += -notches * kWheelLinesPerNotch;
            double whole = 0;
            m_wheelRemainder = std::modf(m_wheelRemainder, &whole);
            ScrollLines(static_cast<int64_t>(whole));
        }
        e.Handled(true);
    }

    // ---- surface ---------------------------------------------------------------------------------------------------

    void CodeView::OnHostSizeChanged()
    {
        double const width = m_host.ActualWidth();
        double const height = m_host.ActualHeight();
        if (width < 1 || height < 1)
        {
            return;
        }
        double scale = 1.0;
        if (auto const xamlRoot = m_host.XamlRoot())
        {
            scale = xamlRoot.RasterizationScale();
        }
        auto const pxW = static_cast<uint32_t>(std::floor(width * scale));
        auto const pxH = static_cast<uint32_t>(std::floor(height * scale));
        if (pxW == 0 || pxH == 0)
        {
            return;
        }
        bool const scaleChanged = scale != m_scale;
        if (!scaleChanged && pxW == m_pxW && pxH == m_pxH && m_surface)
        {
            return;
        }
        m_scale = scale;
        if (scaleChanged || !m_format)
        {
            RebuildFormat();
        }
        m_pxW = pxW;
        m_pxH = pxH;
        m_viewW = pxW / scale;
        m_viewH = pxH / scale;
        if (!RecreateSurface())
        {
            return;
        }
        m_image.Width(m_viewW);
        m_image.Height(m_viewH);
        if (m_followEnd)
        {
            ScrollPos const end = EndPos();
            m_top = end.row;
            m_topSub = end.sub;
        }
        UpdateBars();
        Draw(); // synchronously, so a resize never shows an empty surface
    }

    bool CodeView::RecreateSurface()
    {
        try
        {
            m_surface = winrt::Microsoft::UI::Xaml::Media::Imaging::SurfaceImageSource(static_cast<int>(m_pxW), static_cast<int>(m_pxH), true);
            m_native = m_surface.as<ISurfaceImageSourceNative>();
            m_d3d = nullptr;
            m_image.Source(m_surface);
            return true;
        }
        catch (...)
        {
            FL_LOG_INFO(L"CodeView: creating the surface failed");
            m_surface = nullptr;
            m_native = nullptr;
            return false;
        }
    }

    void CodeView::Invalidate()
    {
        Invalidate(m_gate);
    }

    void CodeView::Invalidate(std::shared_ptr<RedrawGate> const& gate)
    {
        {
            std::lock_guard lock{gate->mutex};
            if (gate->view == nullptr || gate->pending || !gate->ui)
            {
                return;
            }
            gate->pending = true;
        }
        if (!gate->ui.TryEnqueue([gate] {
                CodeView* view = nullptr;
                {
                    std::lock_guard lock{gate->mutex};
                    gate->pending = false;
                    view = gate->view;
                }
                if (view != nullptr)
                {
                    view->Draw();
                }
            }))
        {
            std::lock_guard lock{gate->mutex};
            gate->pending = false;
        }
    }

    // ---- drawing ---------------------------------------------------------------------------------------------------

    void CodeView::FetchLines(uint64_t first, uint64_t count)
    {
        if (m_cacheValid && m_cacheFirst == first && m_cache.size() == count)
        {
            return;
        }
        // Scrolling keeps the rows that stay on screen: each fetch rescans from the nearest index checkpoint, which on a
        // minified single line is up to ~1 MiB per row.
        std::vector<Row> rows(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i)
        {
            uint64_t const line = first + i;
            Row& row = rows[static_cast<size_t>(i)];
            if (m_cacheValid && line >= m_cacheFirst && line - m_cacheFirst < m_cache.size())
            {
                row = std::move(m_cache[static_cast<size_t>(line - m_cacheFirst)]);
            }
            else if (!m_doc->LineText(line, row.text, row.continuation)) // guarded: false once the file is faulted
            {
                break;
            }
        }
        // Rows are numbered by logical line: a continuation segment shares its line's number and is not printed.
        uint64_t number = first;
        if (!m_doc->LogicalLine(first, number))
        {
            number = first; // faulted: the rows are unreadable anyway
        }
        for (size_t i = 0; i < rows.size(); ++i)
        {
            if (i > 0 && !rows[i].continuation)
            {
                ++number;
            }
            rows[i].number = number;
        }
        m_cache = std::move(rows);
        m_cacheFirst = first;
        m_cacheValid = true;
    }

    void CodeView::Draw()
    {
        if (!m_surface || !m_native || !m_format || m_pxW == 0 || m_pxH == 0 || !m_doc)
        {
            return;
        }
        auto& devices = DeviceResources::Get();
        auto const started = std::chrono::steady_clock::now();
        // Own references, taken at once: DeviceResources may release its own at any time (idle trim on this thread).
        winrt::com_ptr<ID3D11Device> d3d;
        d3d.copy_from(devices.D3D());
        winrt::com_ptr<ID2D1Device5> d2dRef;
        d2dRef.copy_from(devices.D2DDevice());
        if (!d3d || !d2dRef)
        {
            return;
        }
        if (d3d != m_d3d)
        {
            auto const dxgi = d3d.as<IDXGIDevice>();
            if (FAILED(m_native->SetDevice(dxgi.get())))
            {
                return;
            }
            m_d3d = d3d;
        }

        RECT const update{0, 0, static_cast<LONG>(m_pxW), static_cast<LONG>(m_pxH)};
        winrt::com_ptr<IDXGISurface> surface;
        POINT offset{};
        HRESULT hr = m_native->BeginDraw(update, surface.put(), &offset);
        if (IsDeviceLoss(hr))
        {
            RecoverFromDeviceLoss(hr);
            return;
        }
        if (FAILED(hr))
        {
            FL_LOG_INFO(L"CodeView: BeginDraw failed: 0x{:08X}", static_cast<uint32_t>(hr));
            return;
        }
        m_retriedAfterLoss = false;

        {
            winrt::com_ptr<ID2D1DeviceContext> dc;
            hr = d2dRef->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc.put());
            winrt::com_ptr<ID2D1Bitmap1> target;
            if (SUCCEEDED(hr))
            {
                auto const props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                           D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                                                           static_cast<float>(96 * m_scale), static_cast<float>(96 * m_scale));
                hr = dc->CreateBitmapFromDxgiSurface(surface.get(), &props, target.put());
            }
            if (SUCCEEDED(hr))
            {
                Palette const& palette = PaletteFor(m_dark);
                Frame frame;
                frame.dc = dc.get();
                auto makeBrush = [&](D2D1_COLOR_F color, winrt::com_ptr<ID2D1SolidColorBrush>& out) { dc->CreateSolidColorBrush(color, out.put()); };
                makeBrush(palette.text, frame.text);
                makeBrush(palette.gutterBackground, frame.gutterBackground);
                makeBrush(palette.gutterText, frame.gutterText);
                makeBrush(palette.separator, frame.separator);
                makeBrush(palette.selection, frame.selection);
                for (size_t i = 0; i < kCategoryCount; ++i)
                {
                    makeBrush(palette.token[i], frame.token[i]);
                }

                dc->SetTarget(target.get());
                dc->SetDpi(static_cast<float>(96 * m_scale), static_cast<float>(96 * m_scale));
                dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
                dc->BeginDraw();
                dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x / m_scale), static_cast<float>(offset.y / m_scale)));
                dc->Clear(palette.background);
                if (frame.text && frame.gutterBackground && frame.gutterText && frame.separator)
                {
                    DrawFrame(frame);
                }
                hr = dc->EndDraw();
                dc->SetTarget(nullptr);
            }
        }
        HRESULT const endHr = m_native->EndDraw();
        if (IsDeviceLoss(endHr))
        {
            RecoverFromDeviceLoss(endHr);
            return;
        }
        if (FAILED(endHr))
        {
            FL_LOG_INFO(L"CodeView: EndDraw failed: 0x{:08X}", static_cast<uint32_t>(endHr));
        }

        if (IsDeviceLoss(hr))
        {
            RecoverFromDeviceLoss(hr);
            return;
        }
        if (FAILED(hr))
        {
            FL_LOG_INFO(L"CodeView: drawing failed: 0x{:08X}", static_cast<uint32_t>(hr));
        }
        UpdateBars(); // the widest line seen so far may have grown
        auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        if (ms >= 8)
        {
            core::perf::Mark("codeview_slow_draw", "ms=" + std::to_string(ms));
        }
    }

    void CodeView::RecoverFromDeviceLoss(HRESULT hr)
    {
        DeviceResources::Get().HandleDeviceLost(hr);
        m_d3d = nullptr;
        if (!std::exchange(m_retriedAfterLoss, true))
        {
            RecreateSurface();
            Invalidate();
        }
    }

    void CodeView::DrawFrame(Frame& frame)
    {
        auto* dc = frame.dc;
        frame.gutterWidth = GutterWidth();
        NormalizeTop();
        uint64_t const first = std::min(m_top, m_lineCount);
        uint64_t const count = std::min<uint64_t>(VisibleRows() + 1, m_lineCount - first);
        FetchLines(first, count);
        if (m_doc->Faulted())
        {
            DrawMessage(frame, std::wstring{Str(L"Text", L"Unreadable")});
            return;
        }
        if (m_styler && count > 0)
        {
            m_styler->EnsureStyledThrough(first + count - 1);
        }

        double const textLeft = frame.gutterWidth + kTextPadLeft;
        double const wrapWidth = m_wrap ? WrapWidth() : 0;
        double const xOrigin = textLeft - (m_wrap ? 0.0 : m_hOffset);
        double y = m_wrap ? -static_cast<double>(m_topSub) * m_lineH : 0.0;
        std::vector<std::pair<double, uint64_t>> marks; // gutter numbers: y of the row's first visual line, logical line
        std::vector<StyledRun> runs;
        for (uint64_t i = 0; i < count && y < m_viewH; ++i)
        {
            Row const& row = m_cache[static_cast<size_t>(i)];
            RowLayout rl;
            bool const laidOut = BuildLayout(row.text, rl, wrapWidth);
            if (laidOut)
            {
                if (m_wrap)
                {
                    if (m_lineCounts.size() >= kLineCountCacheMax)
                    {
                        m_lineCounts.clear();
                    }
                    m_lineCounts[first + i] = rl.lines;
                }
                else
                {
                    m_maxLineW = std::max(m_maxLineW, rl.width);
                }
            }
            if (!row.continuation)
            {
                marks.emplace_back(y, row.number);
            }
            double const originX = xOrigin + static_cast<double>(rl.base) * m_charW;
            bool nextIsContinuation = false;
            if (i + 1 < count)
            {
                nextIsContinuation = m_cache[static_cast<size_t>(i + 1)].continuation;
            }
            else if (HasSelection() && first + i + 1 < m_lineCount && first + i < std::max(m_anchor, m_caret).line)
            {
                std::wstring peek; // the row just below the fetched ones: only the selection stub needs to know
                m_doc->LineText(first + i + 1, peek, nextIsContinuation);
            }
            DrawSelection(frame, first + i, row, rl, originX, y, nextIsContinuation);

            if (laidOut)
            {
                if (m_styler)
                {
                    runs.clear();
                    m_styler->StyleLine(first + i, row.text, runs);
                    for (auto const& run : runs)
                    {
                        auto const category = static_cast<size_t>(run.category);
                        if (run.category == TokenCategory::Default || category >= kCategoryCount || !frame.token[category])
                        {
                            continue;
                        }
                        size_t const from = std::max<size_t>(run.start, rl.base);
                        size_t const to = std::min<size_t>(static_cast<size_t>(run.start) + run.length, rl.base + rl.shown);
                        if (to > from)
                        {
                            rl.layout->SetDrawingEffect(frame.token[category].get(),
                                                        DWRITE_TEXT_RANGE{static_cast<UINT32>(from - rl.base), static_cast<UINT32>(to - from)});
                        }
                    }
                }
                dc->DrawTextLayout(D2D1::Point2F(static_cast<float>(originX), static_cast<float>(y)), rl.layout.get(), frame.text.get(),
                                   D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
            y += static_cast<double>(rl.lines) * m_lineH;
        }
        DrawGutter(frame, marks);
    }

    // Highlight of the part of one row inside the selection. A selection that runs past the end of the row also marks the line
    // break with a cell-wide stub (unless the next row is just the continuation of this one).
    void CodeView::DrawSelection(Frame& frame, uint64_t rowIndex, Row const& row, RowLayout const& rl, double originX, double originY,
                                 bool nextIsContinuation)
    {
        if (!frame.selection || !HasSelection())
        {
            return;
        }
        core::TextPos const from = std::min(m_anchor, m_caret);
        core::TextPos const to = std::max(m_anchor, m_caret);
        if (rowIndex < from.line || rowIndex > to.line)
        {
            return;
        }
        size_t const size = row.text.size();
        size_t const start = rowIndex == from.line ? std::min<size_t>(from.column, size) : 0;
        size_t const end = rowIndex == to.line ? std::min<size_t>(to.column, size) : size;
        auto* dc = frame.dc;

        if (rl.layout)
        {
            size_t const windowStart = std::max(start, rl.base);
            size_t const windowEnd = std::min(end, rl.base + rl.shown);
            if (windowEnd > windowStart)
            {
                UINT32 needed = 0;
                rl.layout->HitTestTextRange(static_cast<UINT32>(windowStart - rl.base), static_cast<UINT32>(windowEnd - windowStart),
                                            static_cast<float>(originX), static_cast<float>(originY), nullptr, 0, &needed);
                std::vector<DWRITE_HIT_TEST_METRICS> boxes(needed);
                if (needed > 0 && SUCCEEDED(rl.layout->HitTestTextRange(static_cast<UINT32>(windowStart - rl.base),
                                                                        static_cast<UINT32>(windowEnd - windowStart), static_cast<float>(originX),
                                                                        static_cast<float>(originY), boxes.data(), needed, &needed)))
                {
                    for (UINT32 i = 0; i < needed; ++i)
                    {
                        auto const& b = boxes[i];
                        dc->FillRectangle(D2D1::RectF(b.left, b.top, b.left + b.width, b.top + b.height), frame.selection.get());
                    }
                }
            }
        }

        if (rowIndex < to.line && !nextIsContinuation && rl.base + rl.shown == size)
        {
            float x = static_cast<float>(originX), y = static_cast<float>(originY);
            if (rl.layout)
            {
                DWRITE_HIT_TEST_METRICS metrics{};
                float px = 0, py = 0;
                BOOL trailing = FALSE;
                if (SUCCEEDED(rl.layout->HitTestTextPosition(static_cast<UINT32>(rl.shown), trailing, &px, &py, &metrics)))
                {
                    x += px;
                    y += py;
                }
            }
            dc->FillRectangle(D2D1::RectF(x, y, x + static_cast<float>(m_charW), y + static_cast<float>(m_lineH)), frame.selection.get());
        }
    }

    void CodeView::DrawGutter(Frame& frame, std::vector<std::pair<double, uint64_t>> const& marks)
    {
        auto* dc = frame.dc;
        float const width = static_cast<float>(frame.gutterWidth);
        float const hairline = static_cast<float>(1.0 / m_scale);
        dc->FillRectangle(D2D1::RectF(0, 0, width, static_cast<float>(m_viewH)), frame.gutterBackground.get());
        dc->FillRectangle(D2D1::RectF(width - hairline, 0, width, static_cast<float>(m_viewH)), frame.separator.get());
        for (auto const& [y, line] : marks)
        {
            std::wstring const number = std::to_wstring(line + 1);
            winrt::com_ptr<IDWriteTextLayout> layout;
            if (FAILED(m_dwrite->CreateTextLayout(number.c_str(), static_cast<UINT32>(number.size()), m_format.get(), 1000.0f,
                                                  static_cast<float>(m_lineH), layout.put())))
            {
                continue;
            }
            double const x = frame.gutterWidth - kGutterPadRight - static_cast<double>(number.size()) * m_charW;
            dc->DrawTextLayout(D2D1::Point2F(static_cast<float>(x), static_cast<float>(y)), layout.get(), frame.gutterText.get(),
                               D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    }

    // ---- selection input -------------------------------------------------------------------------------------------

    core::TextPos CodeView::HitTest(double x, double y)
    {
        if (!m_doc || m_lineCount == 0)
        {
            return {};
        }
        uint64_t const first = std::min(m_top, m_lineCount - 1);
        uint64_t const count = std::min<uint64_t>(VisibleRows() + 1, m_lineCount - first);
        FetchLines(first, count);
        double const wrapWidth = m_wrap ? WrapWidth() : 0;
        double rowY = m_wrap ? -static_cast<double>(m_topSub) * m_lineH : 0.0;
        y = std::max(y, rowY);
        for (size_t i = 0; i < m_cache.size(); ++i)
        {
            Row const& row = m_cache[i];
            RowLayout rl;
            bool const laidOut = BuildLayout(row.text, rl, wrapWidth);
            double const height = static_cast<double>(rl.lines) * m_lineH;
            bool const lastRow = i + 1 == m_cache.size();
            if (y < rowY + height || lastRow)
            {
                size_t column = 0;
                if (laidOut)
                {
                    if (y >= rowY + height)
                    {
                        column = row.text.size(); // below the last visible row
                    }
                    else
                    {
                        double const layoutX = x - TextLeft() + (m_wrap ? 0.0 : m_hOffset) - static_cast<double>(rl.base) * m_charW;
                        BOOL trailing = FALSE, inside = FALSE;
                        DWRITE_HIT_TEST_METRICS metrics{};
                        if (SUCCEEDED(rl.layout->HitTestPoint(static_cast<float>(layoutX), static_cast<float>(y - rowY), &trailing, &inside, &metrics)))
                        {
                            column = rl.base + metrics.textPosition + (trailing ? metrics.length : 0);
                        }
                    }
                }
                return {first + i, static_cast<uint32_t>(std::min(column, row.text.size()))};
            }
            rowY += height;
        }
        return {first, 0};
    }

    void CodeView::SetCaret(core::TextPos pos)
    {
        if (pos != m_caret)
        {
            m_caret = pos;
            Invalidate();
        }
    }

    void CodeView::OnPointerPressed(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e)
    {
        auto const point = e.GetCurrentPoint(m_host);
        if (!point.Properties().IsLeftButtonPressed())
        {
            return;
        }
        m_host.CapturePointer(e.Pointer());
        m_pointerX = point.Position().X;
        m_pointerY = point.Position().Y;
        bool const shift = (e.KeyModifiers() & winrt::Windows::System::VirtualKeyModifiers::Shift) != winrt::Windows::System::VirtualKeyModifiers::None;
        core::TextPos const hit = HitTest(m_pointerX, m_pointerY);
        if (!shift)
        {
            m_anchor = hit;
        }
        m_caret = hit;
        m_dragging = true;
        m_dragTimer.Start();
        Invalidate();
        e.Handled(true);
    }

    void CodeView::OnPointerMoved(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e)
    {
        if (!m_dragging)
        {
            return;
        }
        auto const position = e.GetCurrentPoint(m_host).Position();
        m_pointerX = position.X;
        m_pointerY = position.Y;
        SetCaret(HitTest(m_pointerX, std::clamp(m_pointerY, 0.0, std::max(0.0, m_viewH - 1))));
        e.Handled(true);
    }

    void CodeView::EndDrag()
    {
        if (!std::exchange(m_dragging, false))
        {
            return;
        }
        m_dragTimer.Stop();
        m_host.ReleasePointerCaptures();
    }

    // While the button is held outside the text area the view keeps scrolling that way and the caret follows.
    void CodeView::OnDragTick()
    {
        if (!m_dragging)
        {
            return;
        }
        double over = 0;
        if (m_pointerY < 0)
        {
            over = m_pointerY;
        }
        else if (m_pointerY > m_viewH)
        {
            over = m_pointerY - m_viewH;
        }
        if (over != 0)
        {
            double const rows = std::clamp(over / m_lineH, -kMaxDragRowsPerTick, kMaxDragRowsPerTick);
            ScrollLines(static_cast<int64_t>(rows < 0 ? std::min(-1.0, std::floor(rows)) : std::max(1.0, std::ceil(rows))));
        }
        if (!m_wrap)
        {
            double const left = TextLeft();
            double dx = 0;
            if (m_pointerX < left)
            {
                dx = m_pointerX - left;
            }
            else if (m_pointerX > m_viewW)
            {
                dx = m_pointerX - m_viewW;
            }
            if (dx != 0)
            {
                ScrollHorizontal(std::clamp(dx, -kWheelHorizontalDipPerNotch, kWheelHorizontalDipPerNotch) / 2);
            }
        }
        SetCaret(HitTest(m_pointerX, std::clamp(m_pointerY, 0.0, std::max(0.0, m_viewH - 1))));
    }

    // Double-click: the run of letters, digits and underscores under the pointer.
    void CodeView::SelectWordAt(double x, double y)
    {
        core::TextPos const hit = HitTest(x, y);
        std::wstring text;
        bool continuation = false;
        if (!m_doc || !m_doc->LineText(hit.line, text, continuation))
        {
            return;
        }
        size_t at = std::min<size_t>(hit.column, text.size());
        if (at < text.size() && IsWordUnit(text[at]))
        {
            // on a word
        }
        else if (at > 0 && IsWordUnit(text[at - 1]))
        {
            --at;
        }
        else
        {
            return;
        }
        size_t begin = at, end = at + 1;
        while (begin > 0 && IsWordUnit(text[begin - 1]))
        {
            --begin;
        }
        while (end < text.size() && IsWordUnit(text[end]))
        {
            ++end;
        }
        m_anchor = {hit.line, static_cast<uint32_t>(begin)};
        m_caret = {hit.line, static_cast<uint32_t>(end)};
        Invalidate();
    }

    void CodeView::DrawMessage(Frame& frame, std::wstring const& text)
    {
        winrt::com_ptr<IDWriteTextLayout> layout;
        if (FAILED(m_dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), m_format.get(), static_cast<float>(m_viewW),
                                              static_cast<float>(m_viewH), layout.put())))
        {
            return;
        }
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        frame.dc->DrawTextLayout(D2D1::Point2F(0, 0), layout.get(), frame.gutterText.get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
}
