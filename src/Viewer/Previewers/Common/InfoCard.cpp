#include "pch.h"
#include "Previewers/Common/InfoCard.h"

#include <winrt/Windows.UI.Text.h>

#include "Diagnostics/Log.h"
#include "Previewers/Common/ShellInfo.h"
#include "Util/Str.h"

namespace fastlook::viewer
{
    namespace
    {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        using winrt::Microsoft::UI::Xaml::Media::Brush;

        Brush ThemeBrush(wchar_t const* key)
        {
            try
            {
                return Application::Current().Resources().Lookup(winrt::box_value(key)).as<Brush>();
            }
            catch (winrt::hresult_error const&)
            {
                return nullptr;
            }
        }

        TextBlock MakeText(winrt::hstring const& text, double size, wchar_t const* brushKey)
        {
            TextBlock block;
            block.Text(text);
            block.FontSize(size);
            block.TextWrapping(TextWrapping::Wrap);
            if (auto brush = ThemeBrush(brushKey))
            {
                block.Foreground(brush);
            }
            return block;
        }
    }

    void AddShellRows(InfoCardModel& model, ShellInfo const& info, bool withSizeAndAttributes)
    {
        auto add = [&](wchar_t const* key, std::wstring const& value) {
            if (!value.empty())
            {
                model.rows.push_back({Str(L"Fallback", key), winrt::hstring{value}});
            }
        };
        add(L"Type", info.typeName);
        if (withSizeAndAttributes && info.size)
        {
            add(L"Size", FormatBytes(*info.size));
        }
        if (info.created)
        {
            add(L"Created", FormatDate(*info.created));
        }
        if (info.modified)
        {
            add(L"Modified", FormatDate(*info.modified));
        }
        if (withSizeAndAttributes)
        {
            add(L"Attributes", FormatAttributes(info.attributes));
        }
    }

    UIElement BuildInfoCard(InfoCardModel const& model, std::vector<TextBlock>* valueBlocks)
    {
        StackPanel root;
        root.Orientation(Orientation::Vertical);
        root.HorizontalAlignment(HorizontalAlignment::Center);
        root.VerticalAlignment(VerticalAlignment::Center);
        root.Spacing(12);
        root.Padding(ThicknessHelper::FromUniformLength(24));
        root.MaxWidth(560);

        if (model.image)
        {
            Image image;
            image.Source(model.image);
            image.Width(model.imageSizeDip);
            image.Height(model.imageSizeDip);
            image.Stretch(winrt::Microsoft::UI::Xaml::Media::Stretch::Uniform);
            image.HorizontalAlignment(HorizontalAlignment::Center);
            root.Children().Append(image);
        }

        TextBlock title = MakeText(model.title, 20, L"TextFillColorPrimaryBrush");
        title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        title.TextAlignment(TextAlignment::Center);
        title.IsTextSelectionEnabled(true);
        title.TextTrimming(TextTrimming::CharacterEllipsis);
        title.MaxLines(3);
        root.Children().Append(title);

        if (!model.message.empty())
        {
            TextBlock message = MakeText(model.message, 14, L"TextFillColorSecondaryBrush");
            message.TextAlignment(TextAlignment::Center);
            root.Children().Append(message);
        }

        if (!model.rows.empty())
        {
            Grid grid;
            grid.ColumnSpacing(16);
            grid.RowSpacing(4);
            grid.HorizontalAlignment(HorizontalAlignment::Center);
            ColumnDefinition labelColumn;
            labelColumn.Width(GridLengthHelper::Auto());
            ColumnDefinition valueColumn;
            valueColumn.Width(GridLengthHelper::Auto());
            grid.ColumnDefinitions().Append(labelColumn);
            grid.ColumnDefinitions().Append(valueColumn);

            int rowIndex = 0;
            for (auto const& row : model.rows)
            {
                RowDefinition definition;
                definition.Height(GridLengthHelper::Auto());
                grid.RowDefinitions().Append(definition);

                TextBlock label = MakeText(row.label, 13, L"TextFillColorSecondaryBrush");
                label.HorizontalAlignment(HorizontalAlignment::Right);
                Grid::SetRow(label, rowIndex);
                Grid::SetColumn(label, 0);
                grid.Children().Append(label);

                TextBlock value = MakeText(row.value, 13, L"TextFillColorPrimaryBrush");
                value.IsTextSelectionEnabled(true);
                value.MaxWidth(380);
                Grid::SetRow(value, rowIndex);
                Grid::SetColumn(value, 1);
                grid.Children().Append(value);
                if (valueBlocks)
                {
                    valueBlocks->push_back(value);
                }
                ++rowIndex;
            }
            root.Children().Append(grid);
        }

        if (!model.buttons.empty())
        {
            StackPanel buttons;
            buttons.Orientation(Orientation::Horizontal);
            buttons.Spacing(8);
            buttons.HorizontalAlignment(HorizontalAlignment::Center);
            bool first = true;
            for (auto const& spec : model.buttons)
            {
                winrt::Microsoft::UI::Xaml::Controls::Primitives::ButtonBase button{nullptr};
                if (spec.hyperlink)
                {
                    HyperlinkButton link;
                    link.Content(winrt::box_value(spec.label));
                    button = link;
                }
                else
                {
                    Button plain;
                    plain.Content(winrt::box_value(spec.label));
                    if (first)
                    {
                        try
                        {
                            plain.Style(Application::Current().Resources().Lookup(winrt::box_value(L"AccentButtonStyle")).as<Style>());
                        }
                        catch (winrt::hresult_error const&)
                        {
                            // keep the default style
                        }
                    }
                    button = plain;
                }
                first = false;
                button.Click([action = spec.onClick](auto const&, auto const&) {
                    try
                    {
                        if (action)
                        {
                            action();
                        }
                    }
                    catch (...)
                    {
                        FL_LOG_INFO(L"InfoCard: button handler threw");
                    }
                });
                buttons.Children().Append(button);
            }
            root.Children().Append(buttons);
        }

        return root;
    }
}
