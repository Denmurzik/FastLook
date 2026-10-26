#pragma once

#include <cstdint>
#include <optional>

#include "Ui/Commands.h"

namespace fastlook::core
{
    // Maps a key press in the preview window to a command. `vk` is a Win32 virtual-key code. Modifier combinations that are
    // not listed (including Shift/Alt variants of listed keys) map to nothing. Space and the arrows (navigation) act only in
    // an unpinned preview; a pinned window leaves them to its content.
    std::optional<Command> MapPreviewKey(uint32_t vk, bool ctrl, bool shift, bool alt, bool pinned);
}
