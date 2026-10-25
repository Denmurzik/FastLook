#include "System/AppPaths.h"

#include <Windows.h>
#include <ShlObj.h>
#include <objbase.h>

#include <string>

namespace fastlook::core
{
    AppPaths ResolveAppPaths(std::filesystem::path const& exeDir, std::filesystem::path const& localAppData,
                             std::optional<std::filesystem::path> dataDirOverride)
    {
        AppPaths p;
        p.exeDir = exeDir;
        if (dataDirOverride)
        {
            p.dataDir = *dataDirOverride;
        }
        else
        {
            std::error_code ec;
            if (std::filesystem::exists(exeDir / "portable.txt", ec))
            {
                p.dataDir = exeDir;
                p.portable = true;
            }
            else
            {
                p.dataDir = localAppData / "FastLook";
            }
        }
        p.settingsFile = p.dataDir / "settings.json";
        p.logsDir = p.dataDir / "logs";
        return p;
    }

    AppPaths CurrentAppPaths()
    {
        std::wstring exe(MAX_PATH, L'\0');
        for (;;)
        {
            DWORD n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
            if (n == 0)
            {
                exe.clear();
                break;
            }
            if (n < exe.size())
            {
                exe.resize(n);
                break;
            }
            exe.resize(exe.size() * 2);
        }
        std::filesystem::path exeDir = std::filesystem::path(exe).parent_path();

        std::filesystem::path localAppData;
        PWSTR known = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &known)))
            localAppData = known;
        CoTaskMemFree(known);

        std::optional<std::filesystem::path> over;
        DWORD len = GetEnvironmentVariableW(L"FASTLOOK_DATA_DIR", nullptr, 0);
        if (len > 1)
        {
            std::wstring value(len, L'\0');
            DWORD got = GetEnvironmentVariableW(L"FASTLOOK_DATA_DIR", value.data(), len);
            value.resize(got);
            if (!value.empty())
                over = std::filesystem::path(value);
        }
        return ResolveAppPaths(exeDir, localAppData, over);
    }
}
