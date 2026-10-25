#include "pch.h"
#include "SettingsWindow.xaml.h"
#if __has_include("SettingsWindow.g.cpp")
#include "SettingsWindow.g.cpp"
#endif

#include <dwmapi.h>

#include <winrt/Windows.UI.Xaml.Interop.h>

#include <algorithm>
#include <cmath>

#include "Diagnostics/Log.h"
#include "Util/Str.h"
#include "ViewerHost.h"

#pragma comment(lib, "dwmapi.lib")

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace
{
    constexpr int kDefaultWidth = 960;
    constexpr int kDefaultHeight = 680;
    constexpr double kMonitorFraction = 0.9;
}

namespace winrt::FastLook::implementation
{
    SettingsWindow::SettingsWindow()
    {
        InitializeComponent();

        if (auto native = this->try_as<::IWindowNative>())
        {
            native->get_WindowHandle(&m_hwnd);
        }

        using fastlook::viewer::Str;
        Title(Str(L"Settings", L"WindowTitle"));
        GeneralItem().Content(box_value(Str(L"Settings", L"NavGeneral")));
        PreviewItem().Content(box_value(Str(L"Settings", L"NavPreview")));
        AboutItem().Content(box_value(Str(L"Settings", L"NavAbout")));

        Nav().SelectionChanged([this](NavigationView const&, NavigationViewSelectionChangedEventArgs const& args) {
            if (auto const item = args.SelectedItem().try_as<NavigationViewItem>())
            {
                Navigate(unbox_value_or<hstring>(item.Tag(), L"general"));
            }
        });

        // Sized in DIPs of the monitor the window opens on, centred on its work area.
        auto const appWindow = AppWindow();
        auto const area = Microsoft::UI::Windowing::DisplayArea::GetFromWindowId(appWindow.Id(), Microsoft::UI::Windowing::DisplayAreaFallback::Nearest);
        auto const work = area.WorkArea();
        double const scale = m_hwnd ? GetDpiForWindow(m_hwnd) / static_cast<double>(USER_DEFAULT_SCREEN_DPI) : 1.0;
        int const width = std::min(static_cast<int>(std::lround(kDefaultWidth * scale)), static_cast<int>(work.Width * kMonitorFraction));
        int const height = std::min(static_cast<int>(std::lround(kDefaultHeight * scale)), static_cast<int>(work.Height * kMonitorFraction));
        appWindow.MoveAndResize({work.X + (work.Width - width) / 2, work.Y + (work.Height - height) / 2, width, height});

        ApplyTheme();
    }

    void SettingsWindow::SelectPage(std::wstring_view page)
    {
        auto item = GeneralItem();
        if (page == L"preview")
        {
            item = PreviewItem();
        }
        else if (page == L"about")
        {
            item = AboutItem();
        }
        Nav().SelectedItem(item);
    }

    void SettingsWindow::Navigate(hstring const& tag)
    {
        auto type = xaml_typename<FastLook::SettingsGeneralPage>();
        if (tag == L"preview")
        {
            type = xaml_typename<FastLook::SettingsPreviewPage>();
        }
        else if (tag == L"about")
        {
            type = xaml_typename<FastLook::SettingsAboutPage>();
        }
        ContentFrame().Navigate(type);
        // No back stack: a page is rebuilt from the saved settings every time it is shown.
        ContentFrame().BackStack().Clear();
    }

    void SettingsWindow::ApplyTheme()
    {
        auto& host = fastlook::viewer::ViewerHost::Get();
        auto theme = ElementTheme::Default;
        switch (host.Settings().general.theme)
        {
        case fastlook::core::ThemePref::Light:
            theme = ElementTheme::Light;
            break;
        case fastlook::core::ThemePref::Dark:
            theme = ElementTheme::Dark;
            break;
        case fastlook::core::ThemePref::System:
            break;
        }
        if (auto const root = Content().try_as<FrameworkElement>())
        {
            root.RequestedTheme(theme);
        }
        if (m_hwnd)
        {
            BOOL const dark = host.Dark() ? TRUE : FALSE;
            DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        }
    }
}
