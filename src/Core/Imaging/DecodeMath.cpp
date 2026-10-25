#include "Imaging/DecodeMath.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <cwctype>
#include <string>
#include <cmath>

namespace fastlook::core
{
    namespace
    {
        constexpr int kRotation[] = {0, 0, 0, 180, 180, 90, 90, 270, 270};                      // index = orientation 1..8
        constexpr bool kFlip[] = {false, false, true, false, true, true, false, true, false};

        bool Valid(uint16_t orientation)
        {
            return orientation >= 1 && orientation <= 8;
        }

        uint32_t Clamp(double value, uint32_t limit)
        {
            return static_cast<uint32_t>(std::clamp(std::llround(value), 1LL, static_cast<long long>(std::max(limit, 1u))));
        }
    }

    SizeU ComputeDecodeSize(SizeU native, SizeD maxDip, double scale)
    {
        if (native.w == 0 || native.h == 0)
        {
            return {1, 1};
        }
        double const maxW = maxDip.w * scale;
        double const maxH = maxDip.h * scale;
        double const ratio = std::min({1.0, maxW / native.w, maxH / native.h});
        if (!(ratio > 0.0))
        {
            return {1, 1};
        }
        if (ratio >= 1.0)
        {
            return native;
        }
        return {Clamp(native.w * ratio, native.w), Clamp(native.h * ratio, native.h)};
    }

    SizeU OrientedSize(SizeU native, uint16_t exifOrientation)
    {
        return Valid(exifOrientation) && exifOrientation >= 5 ? SizeU{native.h, native.w} : native;
    }

    int ExifRotationDegrees(uint16_t exifOrientation)
    {
        return Valid(exifOrientation) ? kRotation[exifOrientation] : 0;
    }

    bool ExifFlipsHorizontally(uint16_t exifOrientation)
    {
        return Valid(exifOrientation) && kFlip[exifOrientation];
    }

    uint32_t NormalizeFrameDelayMs(uint32_t delayMs)
    {
        return delayMs < 20 ? 100 : delayMs;
    }

    bool CanAnimateCanvas(SizeU canvas)
    {
        return canvas.w > 0 && canvas.h > 0 && static_cast<uint64_t>(canvas.w) * canvas.h <= kMaxAnimatedPixels;
    }

    std::optional<SizeU> GifLogicalScreenSize(std::span<const uint8_t> file)
    {
        if (file.size() < 10 || std::memcmp(file.data(), "GIF8", 4) != 0 || (file[4] != '7' && file[4] != '9') || file[5] != 'a')
        {
            return std::nullopt;
        }
        return SizeU{static_cast<uint32_t>(file[6] | (file[7] << 8)), static_cast<uint32_t>(file[8] | (file[9] << 8))};
    }

    std::optional<double> ParseSvgLength(std::wstring_view text)
    {
        while (!text.empty() && iswspace(text.front()))
        {
            text.remove_prefix(1);
        }
        while (!text.empty() && iswspace(text.back()))
        {
            text.remove_suffix(1);
        }
        size_t split = 0;
        while (split < text.size() && (text[split] < 128) && (std::isdigit(text[split]) || text[split] == L'.' || text[split] == L'-' ||
                                                              text[split] == L'+' || text[split] == L'e' || text[split] == L'E'))
        {
            ++split;
        }
        // "e" starts a unit ("em", "ex") when no digit follows it.
        if (split > 0 && split < text.size() && (text[split - 1] == L'e' || text[split - 1] == L'E'))
        {
            --split;
        }
        std::string number;
        for (size_t i = 0; i < split; ++i)
        {
            number.push_back(static_cast<char>(text[i]));
        }
        double value = 0.0;
        auto const end = number.data() + number.size();
        auto const parsed = std::from_chars(number.data(), end, value);
        if (number.empty() || parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(value))
        {
            return std::nullopt;
        }
        auto const unit = text.substr(split);
        double factor = 0.0;
        if (unit.empty() || unit == L"px")
        {
            factor = 1.0;
        }
        else if (unit == L"in")
        {
            factor = 96.0;
        }
        else if (unit == L"cm")
        {
            factor = 96.0 / 2.54;
        }
        else if (unit == L"mm")
        {
            factor = 96.0 / 25.4;
        }
        else if (unit == L"pt")
        {
            factor = 96.0 / 72.0;
        }
        else if (unit == L"pc")
        {
            factor = 16.0;
        }
        else
        {
            return std::nullopt; // %, em, ex, ...
        }
        return value * factor;
    }

    SizeD SvgIntrinsicSize(std::optional<double> width, std::optional<double> height, std::optional<SizeD> viewBox)
    {
        constexpr double kDefault = 512.0;
        constexpr double kMax = 16384.0;
        auto const valid = [](std::optional<double> v) { return v && *v > 0.0 && std::isfinite(*v); };
        bool const hasBox = viewBox && viewBox->w > 0.0 && viewBox->h > 0.0 && std::isfinite(viewBox->w) && std::isfinite(viewBox->h);
        double const aspect = hasBox ? viewBox->w / viewBox->h : 1.0;

        SizeD size{kDefault, kDefault};
        if (valid(width) && valid(height))
        {
            size = {*width, *height};
        }
        else if (valid(width))
        {
            size = {*width, hasBox ? *width / aspect : *width};
        }
        else if (valid(height))
        {
            size = {hasBox ? *height * aspect : *height, *height};
        }
        else if (hasBox)
        {
            size = *viewBox;
        }

        double const largest = std::max(size.w, size.h);
        if (largest > kMax)
        {
            size = {size.w * kMax / largest, size.h * kMax / largest};
        }
        return {std::max(size.w, 1.0), std::max(size.h, 1.0)};
    }
}
