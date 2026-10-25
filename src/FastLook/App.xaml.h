#pragma once

#include "App.xaml.g.h"

namespace winrt::FastLook::implementation
{
    struct App : AppT<App>
    {
        App();

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

    private:
        Microsoft::UI::Xaml::Window m_window{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_trimTimer{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_showTimer{ nullptr };
    };
}
