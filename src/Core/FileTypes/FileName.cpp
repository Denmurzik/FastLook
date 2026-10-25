#include "FileTypes/FileName.h"

namespace fastlook::core
{
    std::wstring_view BaseName(std::wstring_view path)
    {
        size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring_view::npos ? path : path.substr(slash + 1);
    }

    std::wstring LowerAscii(std::wstring_view text)
    {
        std::wstring out(text);
        for (wchar_t& c : out)
            if (c >= L'A' && c <= L'Z')
                c = static_cast<wchar_t>(c - L'A' + L'a');
        return out;
    }

    std::wstring LowerExtension(std::wstring_view fileName)
    {
        std::wstring_view name = BaseName(fileName);
        size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring_view::npos)
            return {};
        return LowerAscii(name.substr(dot + 1));
    }
}
