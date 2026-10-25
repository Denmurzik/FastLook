#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace fastlook::core
{
    enum class MissingCodec { Heif, Av1, Raw, Hevc };

    // heic/heif -> Heif, avif -> Av1, camera RAW extensions -> Raw.
    std::optional<MissingCodec> MissingCodecForImage(std::wstring_view fileName);
    std::wstring_view StoreProductId(MissingCodec codec);
    // L"ms-windows-store://pdp/?productid=" + productId
    std::wstring StoreUri(std::wstring_view productId);
}
