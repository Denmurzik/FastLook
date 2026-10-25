#pragma once

#include <optional>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>

#include "Geometry.h"

namespace fastlook::viewer::anim
{
    // Window show/hide/resize. Stubs until the animation task: they apply the final state instantly.

    // False when the system has client-area animations switched off (SPI_GETCLIENTAREAANIMATION).
    bool Enabled();
    // Positions the window at finalPx and shows it (activating it).
    void In(winrt::Microsoft::UI::Xaml::Window const& window, std::optional<core::RectI> iconPx, core::RectI finalPx);
    // Completes when the window may be hidden by the caller.
    winrt::Windows::Foundation::IAsyncAction Out(winrt::Microsoft::UI::Xaml::Window const& window, std::optional<core::RectI> iconPx);
    void Resize(winrt::Microsoft::UI::Xaml::Window const& window, core::RectI fromPx, core::RectI toPx);
}
