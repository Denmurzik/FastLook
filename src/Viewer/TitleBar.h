#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "Ui/Commands.h"

namespace fastlook::viewer
{
    // The 48 px title bar, built in code into the window's TitleBarHost: icon, name, counter, the previewer's toolbar slot and
    // the Open / Open with / Show in folder / Pin / Close buttons. UI thread only.
    class TitleBar : public std::enable_shared_from_this<TitleBar>
    {
    public:
        explicit TitleBar(winrt::Microsoft::UI::Xaml::Controls::Grid const& host);

        // Receives the command of a pressed button.
        std::function<void(core::Command)> onCommand;

        void SetName(winrt::hstring const& name);
        // Hidden unless there is more than one item.
        void SetCounter(size_t index, size_t count);
        // Loads the shell small icon of `path` off the UI thread; a newer call or a failure leaves the previous icon.
        void SetIconFor(std::wstring const& path);
        void SetToolbar(std::vector<winrt::Microsoft::UI::Xaml::UIElement> const& items);
        // A pinned window has no Pin button.
        void SetPinned(bool pinned);
        // Space reserved for the system caption buttons, in DIP.
        void SetRightInset(double dip);

    private:
        winrt::fire_and_forget LoadIconAsync(std::wstring path, uint64_t generation);

        winrt::Microsoft::UI::Dispatching::DispatcherQueue m_ui{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::Image m_icon{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_name{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_counter{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_toolbar{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::Button m_pin{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::Border m_inset{nullptr};
        std::wstring m_iconPath;
        uint64_t m_iconGeneration = 0;
    };
}
