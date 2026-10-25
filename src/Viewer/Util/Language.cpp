#include "pch.h"
#include "Util/Language.h"

#include "Diagnostics/Log.h"

namespace fastlook::viewer
{
    void ApplyLanguage(core::LanguagePref pref, std::optional<core::LanguagePref> cliOverride)
    {
        using core::LanguagePref;

        wchar_t const* tag = L"";
        switch (cliOverride.value_or(pref))
        {
        case LanguagePref::English:
            tag = L"en-US";
            break;
        case LanguagePref::Russian:
            tag = L"ru-RU";
            break;
        case LanguagePref::System:
            break;
        }

        try
        {
            winrt::Microsoft::Windows::Globalization::ApplicationLanguages::PrimaryLanguageOverride(tag);
        }
        catch (winrt::hresult_error const& e)
        {
            FL_LOG_INFO(L"Language override failed: 0x{:08X}", static_cast<uint32_t>(e.code().value));
        }
    }
}
