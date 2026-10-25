#include "pch.h"
#include "AboutProbe.xaml.h"
#if __has_include("AboutProbe.g.cpp")
#include "AboutProbe.g.cpp"
#endif
#include "Util/Str.h"

namespace winrt::FastLook::implementation
{
    AboutProbe::AboutProbe()
    {
        InitializeComponent();
        ProbeText().Text(fastlook::viewer::Str(L"Common", L"Close"));
    }
}
