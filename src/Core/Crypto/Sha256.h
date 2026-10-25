#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace fastlook::core
{
    using Sha256Digest = std::array<uint8_t, 32>;

    Sha256Digest Sha256(std::span<const uint8_t> data);
    // Streams the file in 1 MiB chunks; nullopt when it cannot be read.
    std::optional<Sha256Digest> Sha256File(std::filesystem::path const& path);
    // Reads "<64 hex>  <name>", "<64 hex> *<name>" or a bare "<64 hex>" (first line). A named line must name `expectedFileName`.
    std::optional<Sha256Digest> ParseSha256Line(std::string_view text, std::string_view expectedFileName);
    // Lower-case hex.
    std::string ToHex(Sha256Digest const& digest);
}
