#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
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
        void SetStyler(std::shared_ptr<ILineStyler> styler);
        void SetWrap(bool wrap);
        bool Wrap() const { return m_wrap; }
        void SetFontSize(float dip); // also the size ResetZoom returns to
        float FontSize() const { return m_fontSize; }
        void SetDark(bool dark);
        void ZoomBy(float factor);
        void ResetZoom();

        bool HasSelection() const;
        std::wstring SelectedText() const;
        void SelectAll();

        // Scrolling keys: PgUp / PgDn / Home / End. False when the key is not one of them.
        bool OnScrollKey(winrt::Windows::System::VirtualKey key, bool ctrl, bool shift);

        // UI thread: the line index grew (or finished).
        void OnIndexProgress(uint64_t lines, bool complete);

        struct Palette; // internal to CodeView.cpp
        struct Frame;

    private:
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

        void RebuildFormat();
        void OnHostSizeChanged();
        bool RecreateSurface();
        void RecoverFromDeviceLoss(HRESULT hr);
        void Draw();
        void DrawFrame(Frame& frame);
        void DrawGutter(Frame& frame, uint64_t first, uint64_t last);
        void DrawMessage(Frame& frame, std::wstring const& text);
        void FetchLines(uint64_t first, uint64_t count);

        uint64_t VisibleRows() const;
        uint64_t MaxTop() const;
        double GutterWidth(); // also grows the (never shrinking) digit count
        double HorizontalMax();
        void ScrollLines(int64_t delta);
        void ScrollTo(uint64_t top);
        void ScrollHorizontal(double deltaDip);
        void UpdateBars();
        void OnWheel(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e);

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
        winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBase::ValueChanged_revoker m_vChanged;
        winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBase::ValueChanged_revoker m_hChanged;

        // Surface.
        winrt::Microsoft::UI::Xaml::Media::Imaging::SurfaceImageSource m_surface{nullptr};
        winrt::com_ptr<ISurfaceImageSourceNative> m_native;
        void* m_deviceKey = nullptr;         // the D3D device the surface was bound to
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

        // Text of the lines on screen.
        uint64_t m_cacheFirst = 0;
        std::vector<std::wstring> m_cache;
        bool m_cacheValid = false;
    };
}
