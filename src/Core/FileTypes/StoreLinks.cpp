#include "FileTypes/StoreLinks.h"

#include <algorithm>
#include <array>

#include "FileTypes/FileName.h"

namespace fastlook::core
{
    namespace
    {
        constexpr std::array<std::wstring_view, 19> kRawExtensions{
            L"cr2", L"cr3", L"nef", L"nrw", L"arw", L"srf", L"sr2", L"dng", L"orf", L"rw2",
            L"raf", L"srw", L"pef", L"rwl", L"3fr", L"erf", L"kdc", L"mrw", L"x3f"};
    }

    std::optional<MissingCodec> MissingCodecForImage(std::wstring_view fileName)
    {
        std::wstring ext = LowerExtension(fileName);
        if (ext == L"heic" || ext == L"heif")
            return MissingCodec::Heif;
        if (ext == L"avif")
            return MissingCodec::Av1;
        if (std::find(kRawExtensions.begin(), kRawExtensions.end(), ext) != kRawExtensions.end())
            return MissingCodec::Raw;
        return std::nullopt;
    }

    std::wstring_view StoreProductId(MissingCodec codec)
    {
        switch (codec)
        {
        case MissingCodec::Heif: return L"9PMMSR1CGPWG";
        case MissingCodec::Av1: return L"9MVZQVXJBQ9V";
        case MissingCodec::Raw: return L"9NCTDW2W1BH8";
        case MissingCodec::Hevc: return L"9NMZLZ57R3T7";
        }
        return {};
    }

    std::wstring StoreUri(std::wstring_view productId)
    {
        std::wstring uri = L"ms-windows-store://pdp/?productid=";
        uri.append(productId);
        return uri;
    }
}
