#include "Text/Utf.h"

#include <windows.h>

#include <limits>

namespace fastlook::core
{
    std::string ToUtf8(std::wstring_view text)
    {
        if (text.empty() || text.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return {};

        int const len = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (len <= 0) return {};

        std::string out(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), len, nullptr, nullptr);
        return out;
    }

    std::wstring FromUtf8(std::string_view text)
    {
        if (text.empty() || text.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return {};

        // Without MB_ERR_INVALID_CHARS invalid sequences map to U+FFFD.
        int const len = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (len <= 0) return {};

        std::wstring out(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), len);
        return out;
    }
}
