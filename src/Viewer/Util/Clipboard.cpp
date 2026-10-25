#include "pch.h"
#include "Util/Clipboard.h"

#include "Diagnostics/Log.h"

namespace fastlook::viewer
{
    bool SetClipboardText(HWND owner, std::wstring const& text)
    {
        if (!OpenClipboard(owner))
        {
            FL_LOG_INFO(L"Copying text: OpenClipboard failed, error {}", GetLastError());
            return false;
        }
        bool ok = false;
        if (EmptyClipboard())
        {
            size_t const bytes = (text.size() + 1) * sizeof(wchar_t);
            if (HGLOBAL const memory = GlobalAlloc(GMEM_MOVEABLE, bytes))
            {
                if (void* const data = GlobalLock(memory))
                {
                    memcpy(data, text.c_str(), bytes);
                    GlobalUnlock(memory);
                    ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
                }
                if (!ok)
                {
                    GlobalFree(memory);
                }
            }
        }
        CloseClipboard();
        return ok;
    }
}
