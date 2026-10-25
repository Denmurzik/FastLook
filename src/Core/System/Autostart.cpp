#include "System/Autostart.h"

#include <Windows.h>

#include <string>

namespace fastlook::core
{
    namespace
    {
        constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    }

    bool IsAutostartEnabled(std::wstring_view valueName)
    {
        std::wstring name(valueName);
        DWORD size = 0;
        return RegGetValueW(HKEY_CURRENT_USER, kRunKey, name.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS;
    }

    bool SetAutostart(bool enable, std::filesystem::path const& daemonExe, std::wstring_view valueName)
    {
        std::wstring name(valueName);
        if (!enable)
        {
            LSTATUS rc = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, name.c_str());
            return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
        }
        std::wstring command = L"\"" + daemonExe.wstring() + L"\"";
        DWORD bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
        return RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, name.c_str(), REG_SZ, command.c_str(), bytes) == ERROR_SUCCESS;
    }
}
