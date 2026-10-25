#pragma once

#include "Geometry.h"

namespace fastlook::core
{
    inline constexpr double kMinWindowW = 480, kMinWindowH = 320, kMaxWorkAreaFraction = 0.8, kTitleBarDip = 48;
    inline constexpr SizeD kDefaultTextSize{960, 720}, kDefaultOtherSize{720, 540};

    // Window rectangle in pixels (title bar included), centred in workAreaPx.
    // preferredContentDip is the content size without the title bar; scale = monitor DPI / 96.
    // The window is clamped to [kMinWindowW x kMinWindowH, kMaxWorkAreaFraction of the work area]; with keepAspect the
    // content is shrunk proportionally instead of being clamped per axis.
    RectI FitPreviewWindow(SizeD preferredContentDip, bool keepAspect, RectI workAreaPx, double scale);
}
