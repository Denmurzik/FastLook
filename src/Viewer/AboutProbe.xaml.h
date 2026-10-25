#pragma once

#include "AboutProbe.g.h"

namespace winrt::FastLook::implementation
{
    struct AboutProbe : AboutProbeT<AboutProbe>
    {
        AboutProbe();
    };
}

namespace winrt::FastLook::factory_implementation
{
    struct AboutProbe : AboutProbeT<AboutProbe, implementation::AboutProbe>
    {
    };
}
