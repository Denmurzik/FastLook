#include "pch.h"
#include "SettingsLauncher.h"

#include "Diagnostics/Log.h"
#include "SettingsWindow.xaml.h"
#include "Util/Foreground.h"

namespace fastlook::viewer
{
    namespace
    {
        using winrt::FastLook::implementation::SettingsWindow;

        // Only the UI thread touches these. Intentionally leaked: the window holds WinRT objects that CRT static
        // destruction would release after WinRT teardown.
        struct State
        {
            winrt::FastLook::SettingsWindow window{nullptr};
            std::function<void()> onClosed;
        };
        State& g = *new State;

        void Bring(winrt::FastLook::SettingsWindow const& window)
        {
            window.Activate();
            ForceForeground(winrt::get_self<SettingsWindow>(window)->Hwnd());
        }
    }

    void ShowSettingsWindow(std::wstring_view page, std::function<void()> onClosed)
    {
        if (g.window)
        {
            winrt::get_self<SettingsWindow>(g.window)->SelectPage(page);
            Bring(g.window);
            return;
        }

        try
        {
            auto const window = winrt::make<SettingsWindow>();
            winrt::get_self<SettingsWindow>(window)->SelectPage(page);
            g.window = window;
            g.onClosed = std::move(onClosed);
            window.Closed([](auto const&, auto const&) {
                // Drop the window first: Closed is the end of its life, and onClosed may exit the process.
                g.window = nullptr;
                auto const done = std::exchange(g.onClosed, nullptr);
                if (done)
                {
                    done();
                }
            });
            Bring(window);
        }
        catch (...)
        {
            FL_LOG_INFO(L"Creating the settings window failed");
            g.window = nullptr;
            if (auto const done = std::exchange(g.onClosed, nullptr))
            {
                done();
            }
            else if (onClosed)
            {
                onClosed();
            }
        }
    }

    void CloseSettingsWindow()
    {
        if (!g.window)
        {
            return;
        }
        try
        {
            g.window.Close(); // raises Closed
        }
        catch (...)
        {
            FL_LOG_INFO(L"Closing the settings window failed");
        }
    }

    void RefreshSettingsWindowTheme()
    {
        if (g.window)
        {
            winrt::get_self<SettingsWindow>(g.window)->ApplyTheme();
        }
    }
}
