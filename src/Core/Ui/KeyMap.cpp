#include "Ui/KeyMap.h"

#include <windows.h>

namespace fastlook::core
{
    std::optional<Command> MapPreviewKey(uint32_t vk, bool ctrl, bool shift, bool alt, bool pinned)
    {
        // Ctrl+Shift+= is the physical "+" on layouts where "+" needs Shift.
        if (alt || (shift && !(ctrl && vk == VK_OEM_PLUS)))
        {
            return std::nullopt;
        }

        if (ctrl)
        {
            switch (vk)
            {
            case 'W': return Command::Close;
            case VK_OEM_PLUS:
            case VK_ADD: return Command::ZoomIn;
            case VK_OEM_MINUS:
            case VK_SUBTRACT: return Command::ZoomOut;
            case '0':
            case VK_NUMPAD0: return Command::ZoomReset;
            case 'C': return Command::Copy;
            default: return std::nullopt;
            }
        }

        switch (vk)
        {
        case VK_SPACE: return pinned ? std::nullopt : std::optional{Command::Close};
        case VK_ESCAPE: return Command::Close;
        case VK_LEFT:
        case VK_UP: return Command::Prev;
        case VK_RIGHT:
        case VK_DOWN: return Command::Next;
        case VK_RETURN: return Command::OpenDefault;
        case VK_F11: return Command::ToggleFullscreen;
        default: return std::nullopt;
        }
    }
}
