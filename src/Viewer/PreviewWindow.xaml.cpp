#include "pch.h"
#include "PreviewWindow.xaml.h"
#if __has_include("PreviewWindow.g.cpp")
#include "PreviewWindow.g.cpp"
#endif
#include "Util/Str.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Windowing;

namespace winrt::FastLook::implementation
{
    PreviewWindow::PreviewWindow()
    {
        InitializeComponent();

        if (auto native = this->try_as<::IWindowNative>())
        {
            native->get_WindowHandle(&m_hwnd);
        }

        LoadingText().Text(fastlook::viewer::Str(L"Common", L"Loading"));

        ExtendsContentIntoTitleBar(true);
        SetTitleBar(TitleBarHost());

        auto appWindow = AppWindow();
        appWindow.TitleBar().PreferredHeightOption(TitleBarHeightOption::Tall);
        if (auto presenter = appWindow.Presenter().try_as<OverlappedPresenter>())
        {
            presenter.IsMinimizable(false);
            presenter.IsMaximizable(false);
        }
        // Warm windows stay out of Alt+Tab and the taskbar; the host turns this on while a preview is shown.
        appWindow.IsShownInSwitchers(false);
    }

    double PreviewWindow::Scale() const
    {
        UINT const dpi = m_hwnd ? GetDpiForWindow(m_hwnd) : USER_DEFAULT_SCREEN_DPI;
        return dpi / static_cast<double>(USER_DEFAULT_SCREEN_DPI);
    }

    void PreviewWindow::ShowLoading()
    {
        ClearContent();
        LoadingText().Visibility(Visibility::Visible);
    }

    void PreviewWindow::SetContent(UIElement const& view)
    {
        // Append first: if it throws, the old content (or the Loading text) is still intact.
        ContentHost().Children().Append(view);
        LoadingText().Visibility(Visibility::Collapsed);
        auto const old = std::exchange(m_content, view);
        if (old)
        {
            uint32_t index = 0;
            if (ContentHost().Children().IndexOf(old, index))
            {
                ContentHost().Children().RemoveAt(index);
            }
        }
    }

    void PreviewWindow::ClearContent()
    {
        if (m_content)
        {
            uint32_t index = 0;
            if (ContentHost().Children().IndexOf(m_content, index))
            {
                ContentHost().Children().RemoveAt(index);
            }
            m_content = nullptr;
        }
    }

    void PreviewWindow::SetTitleText(hstring const& text)
    {
        Title(text);
    }

    void PreviewWindow::SetTheme(ElementTheme theme)
    {
        RootGrid().RequestedTheme(theme);
    }
}
