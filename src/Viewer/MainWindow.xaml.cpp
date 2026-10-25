#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#include "Util/Str.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace winrt::FastLook::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();

        auto const name = fastlook::viewer::Str(L"Resources", L"AppDisplayName");
        Title(name);
        TitleText().Text(name);
        HeadingText().Text(name);

        ExtendsContentIntoTitleBar(true);
        SetTitleBar(AppTitleBar());
        AppWindow().Resize({ 800, 500 });
    }
}
