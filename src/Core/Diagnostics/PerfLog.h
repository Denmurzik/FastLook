#pragma once

#include <filesystem>
#include <string_view>

namespace fastlook::core::perf
{
    // Opens (appends to) the perf log. Mark is a no-op until this is called.
    void Open(std::filesystem::path const& file);
    bool Enabled();
    // Writes "<name> qpc=<ticks> freq=<hz> ft=<FILETIME as uint64> <detail>\n".
    void Mark(std::string_view name, std::string_view detail = {});
}
