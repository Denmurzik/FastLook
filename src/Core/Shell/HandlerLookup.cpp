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

        // Handlers known to embed WebView2. Lower-case, with braces. Source: PowerToys
        // src/modules/previewpane/FileExplorerDllExporter/dllmain.cpp (current CLSIDs) and
        // src/modules/previewpane/powerpreview/CLSID.h (SHIM-activate and managed CLSIDs of earlier builds).
        // Edge: "Microsoft PDF Previewer" registered under HKLM\...\PreviewHandlers.
        const std::unordered_set<std::wstring> kBlockedClsids{
            // dllmain.cpp: Bgcode, Gcode, Markdown, Monaco, Pdf, Qoi, Svg
            L"{0e6d5bdd-d5f8-4692-a089-8bb88cdd37f4}", L"{a0257634-8812-4ce8-af11-fa69acaeafae}",
            L"{60789d87-9c3c-44af-b18c-3de2c2820ed3}", L"{d8034cfa-f34b-41fe-ad45-62fcbb52a6da}",
            L"{a5a41cc7-02cb-41d4-8c9b-9087040d6098}", L"{729b72cd-b72e-4fe9-bcbf-e954b33fe699}",
            L"{fcdd4eed-41aa-492f-8a84-31a1546226e0}",
            // CLSID.h: SHIM-activate Svg, Md, Pdf, Gcode, Bgcode, Qoi
            L"{74619bda-a66b-451d-864c-a7726f5fe650}", L"{e0907a95-6f9a-4d1b-a97a-7d9d2648881e}",
            L"{4f6d533b-4185-43a6-ad75-9b20034b14ca}", L"{516cb24f-562f-422f-8b01-6b580474d093}",
            L"{8fae8d5d-6bd1-46b4-a74f-cbebba4c7b62}", L"{f498be36-5c94-4ec9-a65a-ad1cf4c38271}",
            // CLSID.h: managed Svg, Md, Pdf, Gcode, Bgcode, Qoi
            L"{ddee2b8a-6807-48a6-bb20-2338174ff779}", L"{45769bcc-e8fd-42d0-947e-02beef77a1f5}",
            L"{07665729-6243-4746-95b7-79579308d1b2}", L"{ec52dea8-7c9f-4130-a77b-1737d0418507}",
            L"{dd8de316-7b01-48e7-ba21-e92c646704af}", L"{8aa07897-c30b-4543-865b-00a0e5a1b32d}",
            // Edge PDF previewer
            L"{3a84f9c2-6164-485c-a7d9-4b27f8ac009e}"};

        // Managed (mscoree.dll) handlers that were reviewed and are allowed to be hosted.
        const std::unordered_set<std::wstring> kManagedAllowList{};

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

    bool IsBlockedPreviewHandler(std::wstring_view clsid, std::wstring_view serverPath)
    {
        // Fail closed: a handler whose server cannot be resolved is not hosted (own previewer or fallback instead).
        if (serverPath.empty())
            return true;

        if (kBlockedClsids.contains(LowerAscii(clsid)))
            return true;

        // PowerToys File Explorer Preview and the Edge PDF handler live under their install dirs.
        std::wstring path = LowerAscii(serverPath);
        if (path.find(L"\\powertoys\\") != std::wstring::npos || path.find(L"\\microsoft\\edge\\") != std::wstring::npos)
            return true;

        std::wstring_view file = BaseName(path);
        if (file == L"inetcomm.dll" || file == L"mshtml.dll") // MSHTML-based
            return true;
        // Managed (.NET) handlers are registered through mscoree.dll and usually embed WebView2.
        if (file == L"mscoree.dll")
            return !kManagedAllowList.contains(LowerAscii(clsid));
        return false;
    }
}
