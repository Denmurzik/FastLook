#pragma once

#include "App.xaml.g.h"

namespace winrt::FastLook::implementation
{
    struct App : AppT<App>
    {
        App();

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);
    };
}
