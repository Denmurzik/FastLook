#include "pch.h"
#include "PreviewWindow.xaml.h"
#if __has_include("PreviewWindow.g.cpp")
#include "PreviewWindow.g.cpp"
#endif
#include <commctrl.h>

#include <cmath>

#include "Diagnostics/PerfLog.h"
#include "TitleBar.h"
#include "Ui/Placement.h"
#include "Util/Str.h"

#pragma comment(lib, "comctl32.lib")

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Windowing;

namespace
{
    // Enforces the minimum window size on user resize (the presenter has no such setting).
    LRESULT CALLBACK MinSizeProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
    {
        if (message == WM_GETMINMAXINFO)
        {
            LRESULT const result = DefSubclassProc(hwnd, message, wParam, lParam);
            double const scale = GetDpiForWindow(hwnd) / static_cast<double>(USER_DEFAULT_SCREEN_DPI);
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = static_cast<LONG>(std::lround(fastlook::core::kMinWindowW * scale));
            info->ptMinTrackSize.y = static_cast<LONG>(std::lround(fastlook::core::kMinWindowH * scale));
            return result;
        }
        if (message == WM_NCDESTROY)
        {
            RemoveWindowSubclass(hwnd, MinSizeProc, id);
        }
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }

    winrt::Windows::UI::Color Argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
    {
        return {a, r, g, b};
    }
}

namespace winrt::FastLook::implementation
{
    PreviewWindow::PreviewWindow()
    {
        InitializeComponent();
        fastlook::core::perf::Mark("win_xaml_loaded");

        if (auto native = this->try_as<::IWindowNative>())
        {
            native->get_WindowHandle(&m_hwnd);
        }

        if (m_hwnd)
        {
            SetWindowSubclass(m_hwnd, MinSizeProc, 1, 0);
        }

        LoadingText().Text(fastlook::viewer::Str(L"Common", L"Loading"));
        m_titleBar = std::make_shared<fastlook::viewer::TitleBar>(TitleBarHost());
        RootGrid().PreviewKeyDown([this](auto const&, Input::KeyRoutedEventArgs const& e) {
            if (!m_keyHandler)
            {
                return;
            }
            bool const ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
            if (m_keyHandler(static_cast<uint32_t>(e.Key()), ctrl, shift, alt))
            {
                e.Handled(true);
            }
        });

        fastlook::core::perf::Mark("win_before_extends");
        ExtendsContentIntoTitleBar(true);
        fastlook::core::perf::Mark("win_extends_set");
        SetTitleBar(TitleBarHost());
        fastlook::core::perf::Mark("win_titlebar_set");

        auto appWindow = AppWindow();
        appWindow.TitleBar().PreferredHeightOption(TitleBarHeightOption::Tall);
        if (auto presenter = appWindow.Presenter().try_as<OverlappedPresenter>())
        {
            presenter.IsMinimizable(false);
            presenter.IsMaximizable(false);
        }
        // Warm windows stay out of Alt+Tab and the taskbar; the host turns this on while a preview is shown.
        appWindow.IsShownInSwitchers(false);
        appWindow.Changed([this](auto const&, AppWindowChangedEventArgs const& args) {
            // The inset only changes with the presenter (min/max buttons) or the monitor DPI; asking for it on every move/resize
            // of the show path would cost time for nothing.
            if (args.DidPresenterChange() || (args.DidSizeChange() && GetDpiForWindow(m_hwnd) != m_insetDpi))
            {
                UpdateCaptionInset();
            }
        });
        UpdateCaptionInset();
        fastlook::core::perf::Mark("win_ctor_done");
    }

    void PreviewWindow::UpdateCaptionInset()
    {
        // The system caption buttons sit over the right end of the title bar: keep our buttons clear of them.
        m_insetDpi = GetDpiForWindow(m_hwnd);
        m_titleBar->SetRightInset(AppWindow().TitleBar().RightInset() / Scale());
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
        m_titleBar->SetToolbar({});
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
        m_titleBar->SetToolbar({});
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

    void PreviewWindow::SetItem(std::wstring const& path, hstring const& name, size_t index, size_t count)
    {
        Title(name);
        m_titleBar->SetName(name);
        m_titleBar->SetCounter(index, count);
        m_titleBar->SetIconFor(path);
    }

    void PreviewWindow::SetTitle(hstring const& name)
    {
        Title(name);
        m_titleBar->SetName(name);
    }

    void PreviewWindow::SetCounter(size_t index, size_t count)
    {
        m_titleBar->SetCounter(index, count);
    }

    void PreviewWindow::SetToolbar(std::vector<UIElement> const& items)
    {
        m_titleBar->SetToolbar(items);
    }

    void PreviewWindow::SetCommandHandler(std::function<void(fastlook::core::Command)> handler)
    {
        m_titleBar->onCommand = std::move(handler);
    }

    void PreviewWindow::SetKeyHandler(std::function<bool(uint32_t, bool, bool, bool)> handler)
    {
        m_keyHandler = std::move(handler);
    }

    void PreviewWindow::ApplyCaptionColors(bool dark)
    {
        if (m_captionDark == dark)
        {
            return;
        }
        m_captionDark = dark;
        auto titleBar = AppWindow().TitleBar();
        auto const clear = Argb(0, 0, 0, 0);
        if (dark)
        {
            titleBar.ButtonForegroundColor(Argb(255, 255, 255, 255));
            titleBar.ButtonHoverForegroundColor(Argb(255, 255, 255, 255));
            titleBar.ButtonPressedForegroundColor(Argb(255, 255, 255, 255));
            titleBar.ButtonInactiveForegroundColor(Argb(255, 139, 139, 139));
            titleBar.ButtonHoverBackgroundColor(Argb(26, 255, 255, 255));
            titleBar.ButtonPressedBackgroundColor(Argb(15, 255, 255, 255));
        }
        else
        {
            titleBar.ButtonForegroundColor(Argb(255, 26, 26, 26));
            titleBar.ButtonHoverForegroundColor(Argb(255, 26, 26, 26));
            titleBar.ButtonPressedForegroundColor(Argb(255, 26, 26, 26));
            titleBar.ButtonInactiveForegroundColor(Argb(255, 150, 150, 150));
            titleBar.ButtonHoverBackgroundColor(Argb(26, 0, 0, 0));
            titleBar.ButtonPressedBackgroundColor(Argb(15, 0, 0, 0));
        }
        titleBar.ButtonBackgroundColor(clear);
        titleBar.ButtonInactiveBackgroundColor(clear);
    }

    void PreviewWindow::ToggleFullscreen()
    {
        auto appWindow = AppWindow();
        auto const bar = RootGrid().RowDefinitions().GetAt(0);
        if (!m_fullscreen)
        {
            auto const position = appWindow.Position();
            auto const size = appWindow.Size();
            m_windowedBounds = {position.X, position.Y, size.Width, size.Height};
            m_fullscreen = true;
            m_barHeight = bar.Height();
            bar.Height(GridLengthHelper::FromPixels(0));
            TitleBarHost().Visibility(Visibility::Collapsed);
            appWindow.SetPresenter(AppWindowPresenterKind::FullScreen);
        }
        else
        {
            m_fullscreen = false;
            appWindow.SetPresenter(AppWindowPresenterKind::Overlapped);
            if (auto presenter = appWindow.Presenter().try_as<OverlappedPresenter>())
            {
                presenter.IsMinimizable(m_pinned);
                presenter.IsMaximizable(m_pinned);
            }
            appWindow.MoveAndResize(m_windowedBounds);
            bar.Height(m_barHeight); // the height XAML defines
            TitleBarHost().Visibility(Visibility::Visible);
            UpdateCaptionInset();
        }
    }

    void PreviewWindow::SetPinned(bool pinned)
    {
        m_pinned = pinned;
        m_titleBar->SetPinned(pinned);
        auto appWindow = AppWindow();
        if (auto presenter = appWindow.Presenter().try_as<OverlappedPresenter>())
        {
            presenter.IsMinimizable(pinned);
            presenter.IsMaximizable(pinned);
        }
        appWindow.IsShownInSwitchers(pinned);
        UpdateCaptionInset();
        // The system may only lay the caption buttons out after this turn.
        DispatcherQueue().TryEnqueue([weak = get_weak()] {
            if (auto self = weak.get())
            {
                self->UpdateCaptionInset();
            }
        });
    }

    void PreviewWindow::SetTheme(ElementTheme theme)
    {
        RootGrid().RequestedTheme(theme);
    }
}
