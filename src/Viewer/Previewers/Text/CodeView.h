#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.System.h>

#include <d2d1_3.h>
#include <dwrite_3.h>
#include <microsoft.ui.xaml.media.dxinterop.h>

#include "Previewers/Text/ILineStyler.h"
#include "Text/TextDocument.h"

namespace fastlook::viewer
{
    // Virtualized text view: a SurfaceImageSource the size of the viewport (in physical pixels) is redrawn with Direct2D /
    // DirectWrite for the visible lines only, next to a line-number gutter and two ScrollBars. UI thread only, except where noted.
    class CodeView
    {
    public:
        explicit CodeView(std::shared_ptr<core::TextDocument> document);
        ~CodeView();
        CodeView(CodeView const&) = delete;
        CodeView& operator=(CodeView const&) = delete;

        winrt::Microsoft::UI::Xaml::UIElement Root();

        // May be called with null (no highlighting). The styler's `invalidated` callback is set here; it may fire on any thread.
        // Call before the styler starts its worker: `invalidated` is assigned here and never touched again (a stale one only
        // reaches the redraw gate, which is harmless).
        void SetStyler(std::shared_ptr<ILineStyler> styler);
        void SetWrap(bool wrap);
        bool Wrap() const { return m_wrap; }
        void SetFontSize(float dip); // also the size ResetZoom returns to
        float FontSize() const { return m_fontSize; }
        void SetDark(bool dark);
        void ZoomSteps(int steps); // +-1 DIP per step
        void ZoomBy(float factor);
        void ResetZoom();

        // Selection (mouse press-drag, double-click word, Ctrl+A) is kept as document rows (a continuation segment of a longer
        // line is its own row) and UTF-16 columns. SelectedText is capped at 16 Mi UTF-16 units.
        bool HasSelection() const;
        std::wstring SelectedText() const;
        void SelectAll();

        // Scrolling keys: PgUp / PgDn / Home / End. False when the key is not one of them.
        bool OnScrollKey(winrt::Windows::System::VirtualKey key, bool ctrl, bool shift);

        // UI thread: the line index grew (or finished).
        void OnIndexProgress(uint64_t lines, bool complete);

    private:
        struct Palette;
        struct Frame;
        struct Row
        {
            std::wstring text;
            bool continuation = false;
            uint64_t number = 0; // logical line number (0-based); meaningless on a continuation row
        };
        // Scroll position: a document row plus, when wrapping, the visual line inside it.
        struct ScrollPos
        {
            uint64_t row = 0;
            uint32_t sub = 0;
            auto operator<=>(ScrollPos const&) const = default;
        };
        // A laid-out row. Unwrapped rows longer than kWindowedLineUnits only contain the horizontally visible window
        // [base, base + shown).
        struct RowLayout
        {
            winrt::com_ptr<IDWriteTextLayout> layout;
            size_t base = 0, shown = 0;
            double width = 0;
            uint32_t lines = 1;
        };
        struct RedrawGate
        {
            std::mutex mutex;
            CodeView* view = nullptr;
            bool pending = false;
            winrt::Microsoft::UI::Dispatching::DispatcherQueue ui{nullptr};
        };

        void BuildUi();
        void Invalidate(); // any thread
        static void Invalidate(std::shared_ptr<RedrawGate> const& gate);

        static Palette const& PaletteFor(bool dark);
        void SetZoomedSize(float dip);
        void RebuildFormat();
        void OnHostSizeChanged();
        bool RecreateSurface();
        void RecoverFromDeviceLoss(HRESULT hr);
        void Draw();
        void DrawFrame(Frame& frame);
        void DrawGutter(Frame& frame, std::vector<std::pair<double, uint64_t>> const& marks);
        void DrawSelection(Frame& frame, uint64_t rowIndex, Row const& row, RowLayout const& rl, double originX, double originY, bool nextIsContinuation);
        void DrawMessage(Frame& frame, std::wstring const& text);
        void FetchLines(uint64_t first, uint64_t count);

        uint64_t VisibleRows() const;
        uint64_t MaxTop() const;                // unwrapped: the last top row that still fills the viewport
        ScrollPos EndPos();                     // the furthest scroll position (wrapping: bottom of the last row at the viewport bottom)
        double WrapWidth();                     // layout width of wrapped text; drops width-dependent caches when it changes
        double TextLeft();
        bool BuildLayout(std::wstring const& text, RowLayout& out, double wrapWidth);
        uint32_t VisualLines(uint64_t row);     // wrapping: visual lines of a row
        void NormalizeTop();
        void SetTop(uint64_t row, uint32_t sub, bool followEnd = false);
        void ScrollWrapped(int64_t visualLines);
        void ApplyWrapChrome();
        double GutterWidth(); // also grows the (never shrinking) digit count
        double HorizontalMax();
        void ScrollLines(int64_t delta);
        void ScrollTo(uint64_t top, bool followEnd = false);
        void ScrollHorizontal(double deltaDip);
        void UpdateBars();
        void OnWheel(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e);

        // Selection.
        core::TextPos HitTest(double x, double y);
        void OnPointerPressed(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e);
        void OnPointerMoved(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e);
        void EndDrag();
        void OnDragTick();
        void SelectWordAt(double x, double y);
        void SetCaret(core::TextPos pos);

        std::shared_ptr<core::TextDocument> m_doc;
        std::shared_ptr<ILineStyler> m_styler;
        std::shared_ptr<RedrawGate> m_gate;
        bool m_wrap = false;
        bool m_dark = false;
        float m_baseFontSize = 13, m_fontSize = 13;

        // XAML.
        winrt::Microsoft::UI::Xaml::Controls::Grid m_root{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::Grid m_host{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::Image m_image{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::Primitives::ScrollBar m_vbar{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::Primitives::ScrollBar m_hbar{nullptr};
        winrt::Microsoft::UI::Xaml::FrameworkElement::SizeChanged_revoker m_hostSize;
        winrt::Microsoft::UI::Xaml::FrameworkElement::Loaded_revoker m_loaded;
        winrt::Microsoft::UI::Xaml::UIElement::PointerWheelChanged_revoker m_wheel;
        winrt::Microsoft::UI::Xaml::XamlRoot::Changed_revoker m_rootChanged;
        winrt::Microsoft::UI::Xaml::FrameworkElement::ActualThemeChanged_revoker m_themeChanged;
        winrt::Microsoft::UI::Xaml::Media::CompositionTarget::SurfaceContentsLost_revoker m_contentsLost;
        winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBase::ValueChanged_revoker m_vChanged;
        winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBase::ValueChanged_revoker m_hChanged;

        // Surface.
        winrt::Microsoft::UI::Xaml::Media::Imaging::SurfaceImageSource m_surface{nullptr};
        winrt::com_ptr<ISurfaceImageSourceNative> m_native;
        winrt::com_ptr<ID3D11Device> m_d3d;  // the device the surface was bound to (held, so its address cannot be reused)
        uint32_t m_pxW = 0, m_pxH = 0;
        double m_scale = 1.0;
        double m_viewW = 0, m_viewH = 0; // DIP, the image's size
        bool m_retriedAfterLoss = false;

        // Text metrics.
        winrt::com_ptr<IDWriteFactory3> m_dwrite;
        winrt::com_ptr<IDWriteTextFormat> m_format;
        double m_charW = 8, m_lineH = 16;

        // View state.
        uint64_t m_lineCount = 0;
        uint64_t m_top = 0;
        double m_hOffset = 0;
        double m_maxLineW = 0; // widest line laid out so far (DIP)
        uint32_t m_gutterDigits = 3;
        double m_wheelRemainder = 0;
        bool m_updatingBars = false;
        bool m_followEnd = false; // End was pressed: stay at the bottom while the index grows
        uint32_t m_topSub = 0;    // wrapping: visual line of m_top shown at the viewport top
        double m_zoomRemainder = 0;
        double m_wrapWidth = 0;
        std::unordered_map<uint64_t, uint32_t> m_lineCounts; // wrapping: visual lines per row (bounded)
        bool m_endValid = false;
        ScrollPos m_end;
        uint64_t m_endLineCount = 0, m_endRows = 0;
        double m_endWidth = 0;

        // Selection: anchor is where the press happened, caret follows the pointer.
        core::TextPos m_anchor, m_caret;
        bool m_dragging = false;
        double m_pointerX = 0, m_pointerY = 0;
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_dragTimer{nullptr};
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer::Tick_revoker m_dragTick;
        winrt::Microsoft::UI::Xaml::UIElement::PointerPressed_revoker m_pressed;
        winrt::Microsoft::UI::Xaml::UIElement::PointerMoved_revoker m_moved;
        winrt::Microsoft::UI::Xaml::UIElement::PointerReleased_revoker m_released;
        winrt::Microsoft::UI::Xaml::UIElement::PointerCaptureLost_revoker m_captureLost;
        winrt::Microsoft::UI::Xaml::UIElement::DoubleTapped_revoker m_doubleTapped;

        // Text of the lines on screen.
        uint64_t m_cacheFirst = 0;
        std::vector<Row> m_cache;
        bool m_cacheValid = false;
    };
}
