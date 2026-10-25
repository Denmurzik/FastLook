#pragma once

#include <string>

#include <windows.h>

namespace fastlook::viewer
{
    // Puts `text` on the clipboard as CF_UNICODETEXT. False (and a log line) when the clipboard cannot be opened or written.
    bool SetClipboardText(HWND owner, std::wstring const& text);
}
