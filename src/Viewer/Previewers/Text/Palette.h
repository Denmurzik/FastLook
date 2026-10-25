#pragma once

#include <winrt/Windows.UI.h>

#include "Text/TokenCategory.h"

namespace fastlook::viewer
{
    // Syntax colour of a token category on the light or dark code background (VS Code "Light+" / "Dark+" inspired).
    winrt::Windows::UI::Color CategoryColor(core::TokenCategory category, bool dark);
}
