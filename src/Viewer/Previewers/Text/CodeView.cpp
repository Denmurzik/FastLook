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
        winrt::com_ptr<ID2D1SolidColorBrush> text, gutterBackground, gutterText, separator;
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
        m_wrap = wrap; // wrapping itself arrives with the selection/wrap task
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
        Invalidate();
    }

    bool CodeView::HasSelection() const
    {
        return false;
    }

    std::wstring CodeView::SelectedText() const
    {
        return {};
    }

    void CodeView::SelectAll()
    {
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
            ScrollTo(MaxTop(), true);
            return true;
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
            m_top = MaxTop();
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
        double const textWidth = m_viewW - GutterWidth() - kTextPadLeft;
        return std::max(0.0, m_maxLineW + kEndPad - textWidth);
    }

    void CodeView::UpdateBars()
    {
        if (!m_vbar)
        {
            return;
        }
        uint64_t const rows = VisibleRows();
        uint64_t const maxTop = MaxTop();
        m_top = std::min(m_top, maxTop);
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
        constexpr uint64_t kInt64Max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        int64_t const maxTop = static_cast<int64_t>(std::min(MaxTop(), kInt64Max));
        int64_t const target = std::clamp(static_cast<int64_t>(std::min(m_top, kInt64Max)) + delta, int64_t{0}, maxTop);
        ScrollTo(static_cast<uint64_t>(target));
    }

    void CodeView::ScrollTo(uint64_t top, bool followEnd)
    {
        m_followEnd = followEnd;
        top = std::min(top, MaxTop());
        if (top == m_top)
        {
            return;
        }
        m_top = top;
        UpdateBars();
        Invalidate();
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
            ZoomSteps(notches > 0 ? 1 : -1);
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
        double const textWidth = std::max(1.0, m_viewW - textLeft);
        std::vector<StyledRun> runs;
        for (uint64_t i = 0; i < count; ++i)
        {
            std::wstring const& text = m_cache[static_cast<size_t>(i)].text;
            if (text.empty())
            {
                continue;
            }
            std::wstring_view shown = text;
            size_t base = 0;
            double lineWidth = 0;
            if (text.size() > kWindowedLineUnits)
            {
                size_t start = static_cast<size_t>(std::max(0.0, std::floor(m_hOffset / m_charW) - kWindowSlackColumns));
                start = std::min(start, text.size());
                if (start > 0 && start < text.size() && IS_LOW_SURROGATE(text[start]))
                {
                    --start;
                }
                size_t const columns = static_cast<size_t>(std::ceil(textWidth / m_charW)) + 2 * kWindowSlackColumns;
                shown = std::wstring_view{text}.substr(start, columns);
                base = start;
                lineWidth = static_cast<double>(text.size()) * m_charW;
            }

            winrt::com_ptr<IDWriteTextLayout> layout;
            if (FAILED(m_dwrite->CreateTextLayout(shown.data(), static_cast<UINT32>(shown.size()), m_format.get(), 1.0e6f, static_cast<float>(m_lineH),
                                                  layout.put())))
            {
                continue;
            }
            if (lineWidth == 0)
            {
                DWRITE_TEXT_METRICS metrics{};
                layout->GetMetrics(&metrics);
                lineWidth = metrics.widthIncludingTrailingWhitespace;
            }
            m_maxLineW = std::max(m_maxLineW, lineWidth);

            if (m_styler)
            {
                runs.clear();
                m_styler->StyleLine(first + i, text, runs);
                for (auto const& run : runs)
                {
                    auto const category = static_cast<size_t>(run.category);
                    if (run.category == TokenCategory::Default || category >= kCategoryCount || !frame.token[category])
                    {
                        continue;
                    }
                    size_t const from = std::max<size_t>(run.start, base);
                    size_t const to = std::min<size_t>(static_cast<size_t>(run.start) + run.length, base + shown.size());
                    if (to > from)
                    {
                        layout->SetDrawingEffect(frame.token[category].get(), DWRITE_TEXT_RANGE{static_cast<UINT32>(from - base), static_cast<UINT32>(to - from)});
                    }
                }
            }

            D2D1_POINT_2F const origin{static_cast<float>(textLeft - m_hOffset + static_cast<double>(base) * m_charW), static_cast<float>(i * m_lineH)};
            dc->DrawTextLayout(origin, layout.get(), frame.text.get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
        DrawGutter(frame, first, count);
    }

    void CodeView::DrawGutter(Frame& frame, uint64_t first, uint64_t count)
    {
        auto* dc = frame.dc;
        float const width = static_cast<float>(frame.gutterWidth);
        float const hairline = static_cast<float>(1.0 / m_scale);
        dc->FillRectangle(D2D1::RectF(0, 0, width, static_cast<float>(m_viewH)), frame.gutterBackground.get());
        dc->FillRectangle(D2D1::RectF(width - hairline, 0, width, static_cast<float>(m_viewH)), frame.separator.get());
        for (uint64_t i = 0; i < count; ++i)
        {
            if (m_cache[static_cast<size_t>(i)].continuation)
            {
                continue; // a 16 384-unit split of a longer line is a storage artifact, not a line
            }
            std::wstring const number = std::to_wstring(first + i + 1);
            winrt::com_ptr<IDWriteTextLayout> layout;
            if (FAILED(m_dwrite->CreateTextLayout(number.c_str(), static_cast<UINT32>(number.size()), m_format.get(), 1000.0f,
                                                  static_cast<float>(m_lineH), layout.put())))
            {
                continue;
            }
            double const x = frame.gutterWidth - kGutterPadRight - static_cast<double>(number.size()) * m_charW;
            dc->DrawTextLayout(D2D1::Point2F(static_cast<float>(x), static_cast<float>(i * m_lineH)), layout.get(), frame.gutterText.get(),
                               D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
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
