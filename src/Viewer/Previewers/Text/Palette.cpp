#include "pch.h"
#include "Previewers/Text/Palette.h"

#include <array>

namespace fastlook::viewer
{
    namespace
    {
        using core::TokenCategory;

        constexpr size_t kCategoryCount = static_cast<size_t>(TokenCategory::Error) + 1;

        constexpr winrt::Windows::UI::Color Rgb(uint32_t rgb)
        {
            return {255, static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb)};
        }

        // Indexed by TokenCategory: Default, Comment, String, Number, Keyword, Preprocessor, Operator, Identifier, Error.
        constexpr std::array<winrt::Windows::UI::Color, kCategoryCount> kLight{
            Rgb(0x1F1F1F), Rgb(0x008000), Rgb(0xA31515), Rgb(0x098658), Rgb(0x0000FF), Rgb(0xAF00DB), Rgb(0x1F1F1F), Rgb(0x001080), Rgb(0xE51400)};
        constexpr std::array<winrt::Windows::UI::Color, kCategoryCount> kDark{
            Rgb(0xD4D4D4), Rgb(0x6A9955), Rgb(0xCE9178), Rgb(0xB5CEA8), Rgb(0x569CD6), Rgb(0xC586C0), Rgb(0xD4D4D4), Rgb(0x9CDCFE), Rgb(0xF44747)};
    }

    winrt::Windows::UI::Color CategoryColor(TokenCategory category, bool dark)
    {
        auto const index = static_cast<size_t>(category);
        auto const& table = dark ? kDark : kLight;
        return table[index < kCategoryCount ? index : 0];
    }
}
