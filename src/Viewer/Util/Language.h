#pragma once

#include <optional>

#include "Settings/Settings.h"

namespace fastlook::viewer
{
    // Sets the UI language for resource lookups: the command-line override wins over the setting; System = OS language.
    // Must run before the first Str() call (the resource loaders are cached).
    void ApplyLanguage(core::LanguagePref pref, std::optional<core::LanguagePref> cliOverride);
}
