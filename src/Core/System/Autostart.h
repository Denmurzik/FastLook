#pragma once

#include <filesystem>
#include <string_view>

namespace fastlook::core
{
    inline constexpr wchar_t kRunValueName[] = L"FastLook";

    // HKCU\Software\Microsoft\Windows\CurrentVersion\Run\<valueName>
    bool IsAutostartEnabled(std::wstring_view valueName = kRunValueName);
    // Enabling stores the quoted daemonExe path; disabling removes the value (absent counts as success).
    bool SetAutostart(bool enable, std::filesystem::path const& daemonExe, std::wstring_view valueName = kRunValueName);
}
