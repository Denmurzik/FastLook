#include "Shell/HandlerLookup.h"

#include <Windows.h>
#include <objbase.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <mutex>
#include <unordered_set>
#include <vector>

#include "FileTypes/FileName.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

namespace fastlook::core
{
    namespace
    {
        constexpr wchar_t kPreviewHandlerIid[] = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";

        // ".png" for "png", ".png" for ".PNG"; empty when there is no extension.
        std::wstring DottedLowerExtension(std::wstring_view ext)
        {
            if (!ext.empty() && ext.front() == L'.')
                ext.remove_prefix(1);
            if (ext.empty())
                return {};
            return L"." + LowerAscii(ext);
        }

        std::wstring ReadDefaultValue(std::wstring const& subKey)
        {
            DWORD bytes = 0;
            constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
            if (RegGetValueW(HKEY_CLASSES_ROOT, subKey.c_str(), nullptr, flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t))
                return {};
            std::wstring value(bytes / sizeof(wchar_t), L'\0');
            if (RegGetValueW(HKEY_CLASSES_ROOT, subKey.c_str(), nullptr, flags, nullptr, value.data(), &bytes) != ERROR_SUCCESS)
                return {};
            value.resize(wcsnlen(value.c_str(), value.size()));
            return value;
        }

        // LocalServer32 holds a command line: keep only the executable.
        std::wstring ExecutableOf(std::wstring command)
        {
            size_t start = command.find_first_not_of(L' ');
            if (start == std::wstring::npos)
                return {};
            command.erase(0, start);
            if (command.front() == L'"')
            {
                size_t close = command.find(L'"', 1);
                return command.substr(1, close == std::wstring::npos ? std::wstring::npos : close - 1);
            }
            std::wstring lower = LowerAscii(command);
            size_t exe = lower.find(L".exe");
            return exe == std::wstring::npos ? command : command.substr(0, exe + 4);
        }

        struct WicExtensions
        {
            std::mutex mutex;
            bool loaded = false;
            std::unordered_set<std::wstring> extensions;
        };

        WicExtensions& Wic()
        {
            static WicExtensions state;
            return state;
        }

        bool LoadWicExtensions(std::unordered_set<std::wstring>& out)
        {
            using Microsoft::WRL::ComPtr;
            ComPtr<IWICImagingFactory> factory;
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
                return false;
            ComPtr<IEnumUnknown> decoders;
            if (FAILED(factory->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateDefault, &decoders)))
                return false;

            for (;;)
            {
                ComPtr<IUnknown> unknown;
                ULONG fetched = 0;
                if (decoders->Next(1, &unknown, &fetched) != S_OK || fetched == 0)
                    break;
                ComPtr<IWICBitmapDecoderInfo> info;
                if (FAILED(unknown.As(&info)))
                    continue;
                UINT chars = 0;
                if (FAILED(info->GetFileExtensions(0, nullptr, &chars)) || chars == 0)
                    continue;
                std::wstring list(chars, L'\0');
                if (FAILED(info->GetFileExtensions(chars, list.data(), &chars)))
                    continue;
                list.resize(wcsnlen(list.c_str(), list.size()));

                size_t pos = 0;
                while (pos <= list.size())
                {
                    size_t comma = list.find(L',', pos);
                    std::wstring_view token = std::wstring_view(list).substr(pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos);
                    std::wstring ext = DottedLowerExtension(token);
                    if (!ext.empty())
                        out.insert(std::move(ext));
                    if (comma == std::wstring::npos)
                        break;
                    pos = comma + 1;
                }
            }
            return true;
        }
    }

    PerceivedKind GetPerceivedKind(std::wstring_view ext)
    {
        std::wstring dotted = DottedLowerExtension(ext);
        if (dotted.empty())
            return PerceivedKind::Unknown;
        PERCEIVED type = PERCEIVED_TYPE_UNKNOWN;
        PERCEIVEDFLAG flags = 0;
        if (FAILED(AssocGetPerceivedType(dotted.c_str(), &type, &flags, nullptr)))
            return PerceivedKind::Unknown;
        switch (type)
        {
        case PERCEIVED_TYPE_IMAGE: return PerceivedKind::Image;
        case PERCEIVED_TYPE_TEXT: return PerceivedKind::Text;
        case PERCEIVED_TYPE_AUDIO: return PerceivedKind::Audio;
        case PERCEIVED_TYPE_VIDEO: return PerceivedKind::Video;
        case PERCEIVED_TYPE_COMPRESSED: return PerceivedKind::Compressed;
        case PERCEIVED_TYPE_DOCUMENT: return PerceivedKind::Document;
        case PERCEIVED_TYPE_UNSPECIFIED:
        case PERCEIVED_TYPE_UNKNOWN: return PerceivedKind::Unknown;
        default: return PerceivedKind::Other;
        }
    }

    bool HasWicDecoder(std::wstring_view ext)
    {
        std::wstring dotted = DottedLowerExtension(ext);
        if (dotted.empty())
            return false;
        auto& wic = Wic();
        std::lock_guard lock(wic.mutex);
        if (!wic.loaded)
        {
            std::unordered_set<std::wstring> loaded;
            if (!LoadWicExtensions(loaded))
                return false; // no COM on this thread: do not cache the failure
            wic.extensions = std::move(loaded);
            wic.loaded = true;
        }
        return wic.extensions.contains(dotted);
    }

    std::optional<std::wstring> FindPreviewHandlerClsid(std::wstring_view ext)
    {
        std::wstring dotted = DottedLowerExtension(ext);
        if (dotted.empty())
            return std::nullopt;
        wchar_t buffer[96]{};
        DWORD chars = static_cast<DWORD>(std::size(buffer));
        if (FAILED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_SHELLEXTENSION, dotted.c_str(), kPreviewHandlerIid, buffer, &chars)) || buffer[0] == L'\0')
            return std::nullopt;
        return std::wstring(buffer);
    }

    std::wstring HandlerServerPath(std::wstring_view clsid)
    {
        std::wstring base = L"CLSID\\" + std::wstring(clsid);
        std::wstring inproc = ReadDefaultValue(base + L"\\InprocServer32");
        if (!inproc.empty())
            return inproc;
        return ExecutableOf(ReadDefaultValue(base + L"\\LocalServer32"));
    }

    bool IsBlockedPreviewHandler(std::wstring_view, std::wstring_view serverPath)
    {
        // PowerToys File Explorer Preview and the Edge PDF handler live under their install dirs; the
        // MSHTML-based handlers load a browser engine. A path rule covers every CLSID registered here.
        std::wstring path = LowerAscii(serverPath);
        if (path.find(L"\\powertoys\\") != std::wstring::npos || path.find(L"\\microsoft\\edge\\") != std::wstring::npos)
            return true;
        std::wstring_view file = BaseName(path);
        return file == L"inetcomm.dll" || file == L"mshtml.dll";
    }
}
