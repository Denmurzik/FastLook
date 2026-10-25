#pragma once

#include <chrono>
#include <filesystem>
#include <string>

namespace fastlook::viewer
{
    struct AudioInfo
    {
        std::wstring title, artist, album;
        std::chrono::milliseconds duration{};
    };

    // Background thread. Reads the shell property store (PKEY_Title, PKEY_Music_Artist, PKEY_Music_AlbumTitle,
    // PKEY_Media_Duration); missing properties stay empty / zero.
    AudioInfo ReadAudioInfo(std::filesystem::path const& path);
}
