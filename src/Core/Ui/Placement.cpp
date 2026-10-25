#include "Ui/Placement.h"

#include <algorithm>
#include <cmath>

namespace fastlook::core
{
    namespace
    {
        int32_t Round(double v)
        {
            return static_cast<int32_t>(std::lround(v));
        }
    }

    RectI FitPreviewWindow(SizeD preferredContentDip, bool keepAspect, RectI workAreaPx, double scale)
    {
        if (scale <= 0)
        {
            scale = 1.0;
        }

        double const titlePx = kTitleBarDip * scale;
        double const maxContentW = std::max(1.0, workAreaPx.w * kMaxWorkAreaFraction);
        double const maxContentH = std::max(1.0, workAreaPx.h * kMaxWorkAreaFraction - titlePx);

        double contentW = std::max(1.0, preferredContentDip.w * scale);
        double contentH = std::max(1.0, preferredContentDip.h * scale);
        if (keepAspect)
        {
            double const factor = std::min({1.0, maxContentW / contentW, maxContentH / contentH});
            contentW *= factor;
            contentH *= factor;
        }
        else
        {
            contentW = std::min(contentW, maxContentW);
            contentH = std::min(contentH, maxContentH);
        }

        double w = std::max(contentW, kMinWindowW * scale);
        double h = std::max(contentH + titlePx, kMinWindowH * scale);
        // A tiny work area wins over the minimum size: the window never leaves it.
        w = std::min(w, static_cast<double>(workAreaPx.w));
        h = std::min(h, static_cast<double>(workAreaPx.h));

        RectI r;
        r.w = Round(w);
        r.h = Round(h);
        r.x = workAreaPx.x + (workAreaPx.w - r.w) / 2;
        r.y = workAreaPx.y + (workAreaPx.h - r.h) / 2;
        return r;
    }
}
