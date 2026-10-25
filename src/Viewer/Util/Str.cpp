#include "pch.h"
#include "Util/Str.h"

#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>

#include <map>
#include <mutex>
#include <set>

namespace fastlook::viewer
{
    namespace
    {
        using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;

        std::mutex g_mutex;
        std::map<std::wstring, ResourceLoader, std::less<>> g_loaders;
        std::set<std::wstring, std::less<>> g_reportedMissing;

        ResourceLoader LoaderFor(std::wstring_view map)
        {
            auto it = g_loaders.find(map);
            if (it == g_loaders.end())
            {
                ResourceLoader loader{ ResourceLoader::GetDefaultResourceFilePath(), winrt::hstring{ map } };
                it = g_loaders.emplace(std::wstring{ map }, std::move(loader)).first;
            }
            return it->second;
        }
    }

    winrt::hstring Str(std::wstring_view map, std::wstring_view key)
    {
        std::lock_guard lock{ g_mutex };

        winrt::hstring value;
        try
        {
            value = LoaderFor(map).GetString(winrt::hstring{ key });
        }
        catch (winrt::hresult_error const&)
        {
            // Treated like an empty result: reported as missing below.
        }

        if (!value.empty())
        {
            return value;
        }

        std::wstring id{ map };
        id += L'/';
        id += key;
        if (g_reportedMissing.insert(id).second)
        {
            OutputDebugStringW((L"FastLook: missing string resource " + id + L"\n").c_str());
        }
        return winrt::hstring{ L"!" + std::wstring{ key } };
    }
}
