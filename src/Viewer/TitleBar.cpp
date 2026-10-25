#include "pch.h"
#include "TitleBar.h"

#include <robuffer.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>

#include <algorithm>

#include "Diagnostics/Log.h"
#include "Util/Async.h"
#include "Util/Str.h"

#pragma comment(lib, "gdi32.lib")

namespace fastlook::viewer
{
    namespace
    {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        using core::Command;

        constexpr wchar_t kGlyphOpen[] = L"";
        constexpr wchar_t kGlyphOpenWith[] = L"";
        constexpr wchar_t kGlyphFolder[] = L"";
        constexpr wchar_t kGlyphPin[] = L"";
        constexpr wchar_t kGlyphClose[] = L"";

        FontIcon MakeGlyph(wchar_t const* glyph)
        {
            FontIcon icon;
            icon.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily{L"Segoe Fluent Icons, Segoe MDL2 Assets"});
            icon.Glyph(glyph);
            icon.FontSize(16);
            return icon;
        }

        Button MakeButton(winrt::hstring const& tooltip)
        {
            Button button;
            button.Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{winrt::Windows::UI::Colors::Transparent()});
            button.BorderThickness({0, 0, 0, 0});
            button.Padding({10, 6, 10, 6});
            button.MinWidth(36);
            button.VerticalAlignment(VerticalAlignment::Center);
            button.IsTabStop(false);
            ToolTipService::SetToolTip(button, winrt::box_value(tooltip));
            winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(button, tooltip);
            return button;
        }

        // The shell small icon as premultiplied BGRA, size x size (empty on failure). Drawn once over black and once over white
        // to recover the alpha of icons that have no alpha channel of their own.
        std::vector<uint8_t> RenderShellIcon(std::wstring const& path, int size)
        {
            SHFILEINFOW info{};
            if (!SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_SMALLICON) || info.hIcon == nullptr)
            {
                return {};
            }
            std::vector<uint8_t> result;
            HDC const screen = GetDC(nullptr);
            HDC const dc = CreateCompatibleDC(screen);
            BITMAPINFO bmi{};
            bmi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
            size_t const count = static_cast<size_t>(size) * size;

            auto draw = [&](uint32_t background, std::vector<uint8_t>& out) {
                void* bits = nullptr;
                HBITMAP const bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
                if (!bitmap)
                {
                    return false;
                }
                auto const old = SelectObject(dc, bitmap);
                auto* pixels = static_cast<uint32_t*>(bits);
                std::fill(pixels, pixels + count, background);
                DrawIconEx(dc, 0, 0, info.hIcon, size, size, 0, nullptr, DI_NORMAL);
                out.assign(static_cast<uint8_t*>(bits), static_cast<uint8_t*>(bits) + count * 4);
                SelectObject(dc, old);
                DeleteObject(bitmap);
                return true;
            };

            std::vector<uint8_t> onBlack, onWhite;
            if (draw(0xFF000000, onBlack) && draw(0xFFFFFFFF, onWhite))
            {
                result = std::move(onBlack);
                for (size_t i = 0; i < result.size(); i += 4)
                {
                    int const spread = std::max({onWhite[i] - result[i], onWhite[i + 1] - result[i + 1], onWhite[i + 2] - result[i + 2]});
                    result[i + 3] = static_cast<uint8_t>(255 - std::clamp(spread, 0, 255)); // colour over black is already premultiplied
                }
            }
            DeleteDC(dc);
            ReleaseDC(nullptr, screen);
            DestroyIcon(info.hIcon);
            return result;
        }
    }

    TitleBar::TitleBar(Grid const& host)
    {
        m_ui = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();

        for (int i = 0; i < 6; ++i)
        {
            ColumnDefinition column;
            column.Width(i == 1 ? GridLengthHelper::FromValueAndType(1, GridUnitType::Star) : GridLengthHelper::Auto());
            host.ColumnDefinitions().Append(column);
        }

        m_icon = Image{};
        m_icon.Width(16);
        m_icon.Height(16);
        m_icon.Margin({16, 0, 10, 0});
        m_icon.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(m_icon, 0);
        host.Children().Append(m_icon);

        m_name = TextBlock{};
        m_name.VerticalAlignment(VerticalAlignment::Center);
        m_name.TextTrimming(TextTrimming::CharacterEllipsis);
        m_name.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        Grid::SetColumn(m_name, 1);
        host.Children().Append(m_name);

        m_counter = TextBlock{};
        m_counter.VerticalAlignment(VerticalAlignment::Center);
        m_counter.Margin({10, 0, 10, 0});
        m_counter.Opacity(0.7);
        m_counter.Visibility(Visibility::Collapsed);
        Grid::SetColumn(m_counter, 2);
        host.Children().Append(m_counter);

        m_toolbar = StackPanel{};
        m_toolbar.Orientation(Orientation::Horizontal);
        m_toolbar.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(m_toolbar, 3);
        host.Children().Append(m_toolbar);

        StackPanel buttons;
        buttons.Orientation(Orientation::Horizontal);
        buttons.VerticalAlignment(VerticalAlignment::Center);
        buttons.Margin({8, 0, 4, 0});
        Grid::SetColumn(buttons, 4);
        host.Children().Append(buttons);

        auto const raise = [this](Command command) {
            if (onCommand)
            {
                onCommand(command);
            }
        };

        {
            auto open = MakeButton(Str(L"Common", L"Open"));
            StackPanel content;
            content.Orientation(Orientation::Horizontal);
            content.Spacing(8);
            content.Children().Append(MakeGlyph(kGlyphOpen));
            TextBlock label;
            label.Text(Str(L"Common", L"Open"));
            label.VerticalAlignment(VerticalAlignment::Center);
            content.Children().Append(label);
            open.Content(content);
            open.Click([raise](auto const&, auto const&) { raise(Command::OpenDefault); });
            buttons.Children().Append(open);
        }
        {
            auto openWith = MakeButton(Str(L"Common", L"OpenWith"));
            openWith.Content(MakeGlyph(kGlyphOpenWith));
            MenuFlyout flyout;
            MenuFlyoutItem item;
            item.Text(Str(L"Common", L"OpenWith"));
            item.Click([raise](auto const&, auto const&) { raise(Command::OpenWith); });
            flyout.Items().Append(item);
            openWith.Flyout(flyout);
            buttons.Children().Append(openWith);
        }
        {
            auto reveal = MakeButton(Str(L"Common", L"ShowInFolder"));
            reveal.Content(MakeGlyph(kGlyphFolder));
            reveal.Click([raise](auto const&, auto const&) { raise(Command::Reveal); });
            buttons.Children().Append(reveal);
        }
        {
            m_pin = MakeButton(Str(L"Common", L"Pin"));
            m_pin.Content(MakeGlyph(kGlyphPin));
            m_pin.Click([raise](auto const&, auto const&) { raise(Command::Pin); });
            buttons.Children().Append(m_pin);
        }
        {
            auto close = MakeButton(Str(L"Common", L"Close"));
            close.Content(MakeGlyph(kGlyphClose));
            close.Click([raise](auto const&, auto const&) { raise(Command::Close); });
            buttons.Children().Append(close);
        }

        m_inset = Border{};
        m_inset.Width(0);
        Grid::SetColumn(m_inset, 5);
        host.Children().Append(m_inset);
    }

    void TitleBar::SetName(winrt::hstring const& name)
    {
        m_name.Text(name);
    }

    void TitleBar::SetCounter(size_t index, size_t count)
    {
        if (count <= 1)
        {
            m_counter.Visibility(Visibility::Collapsed);
            return;
        }
        std::wstring text{Str(L"Common", L"Counter")};
        auto const replace = [&](std::wstring_view token, size_t value) {
            if (auto const at = text.find(token); at != std::wstring::npos)
            {
                text.replace(at, token.size(), std::to_wstring(value));
            }
        };
        replace(L"{0}", index + 1);
        replace(L"{1}", count);
        m_counter.Text(text);
        m_counter.Visibility(Visibility::Visible);
    }

    void TitleBar::SetIconFor(std::wstring const& path)
    {
        if (path == m_iconPath)
        {
            return;
        }
        m_iconPath = path;
        LoadIconAsync(path, ++m_iconGeneration);
    }

    winrt::fire_and_forget TitleBar::LoadIconAsync(std::wstring path, uint64_t generation)
    {
        auto self = shared_from_this();
        auto const ui = m_ui;
        // A fire_and_forget coroutine terminates the process on an escaping exception: nothing may leave this try block.
        try
        {
            int const size = std::max(16, GetSystemMetrics(SM_CXSMICON));
            co_await winrt::resume_background();
            auto pixels = RenderShellIcon(path, size);
            if (!co_await ResumeOn(ui))
            {
                co_return;
            }
            if (generation != m_iconGeneration)
            {
                co_return;
            }
            if (pixels.empty())
            {
                // No icon: show none and let the next SetIconFor for this path retry.
                m_icon.Source(nullptr);
                m_iconPath.clear();
                co_return;
            }
            winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap bitmap{size, size};
            auto access = bitmap.PixelBuffer().as<::Windows::Storage::Streams::IBufferByteAccess>();
            uint8_t* target = nullptr;
            if (SUCCEEDED(access->Buffer(&target)) && target)
            {
                std::copy(pixels.begin(), pixels.end(), target);
                bitmap.Invalidate();
                m_icon.Source(bitmap);
            }
        }
        catch (...)
        {
            FL_LOG_INFO(L"Loading the title bar icon failed");
        }
    }

    void TitleBar::SetToolbar(std::vector<UIElement> const& items)
    {
        m_toolbar.Children().Clear();
        for (auto const& item : items)
        {
            m_toolbar.Children().Append(item);
        }
    }

    void TitleBar::SetPinned(bool pinned)
    {
        m_pin.Visibility(pinned ? Visibility::Collapsed : Visibility::Visible);
    }

    void TitleBar::SetRightInset(double dip)
    {
        m_inset.Width(std::max(0.0, dip));
    }
}
