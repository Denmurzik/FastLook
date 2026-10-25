#include "pch.h"
#include "WindowAnimator.h"

namespace fastlook::viewer::anim
{
    namespace
    {
        void Place(winrt::Microsoft::UI::Xaml::Window const& window, core::RectI r)
        {
            window.AppWindow().MoveAndResize({r.x, r.y, r.w, r.h});
        }
    }

    bool Enabled()
    {
        BOOL on = TRUE;
        return !SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) || on != FALSE;
    }

    void In(winrt::Microsoft::UI::Xaml::Window const& window, std::optional<core::RectI>, core::RectI finalPx)
    {
        Place(window, finalPx);
        window.AppWindow().Show(true);
    }

    winrt::Windows::Foundation::IAsyncAction Out(winrt::Microsoft::UI::Xaml::Window const&, std::optional<core::RectI>)
    {
        co_return;
    }

    void Resize(winrt::Microsoft::UI::Xaml::Window const& window, core::RectI, core::RectI toPx)
    {
        Place(window, toPx);
    }
}
