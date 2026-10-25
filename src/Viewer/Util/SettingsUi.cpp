#include "pch.h"
#include "Util/SettingsUi.h"

#include "Util/Str.h"
#include "ViewerHost.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace fastlook::viewer
{
    bool CommitSettings(std::function<void(core::Settings&)> const& edit)
    {
        auto& host = ViewerHost::Get();
        auto settings = host.Settings();
        edit(settings);
        return host.SaveSettings(settings);
    }

    Grid MakeSettingsRow(hstring const& title, hstring const& description, UIElement const& control)
    {
        // Loaded from markup so the card brushes are theme resources and follow theme changes.
        auto const grid = Markup::XamlReader::Load(LR"(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
      Padding="16,12" MinHeight="68" CornerRadius="4" BorderThickness="1" ColumnSpacing="16"
      Background="{ThemeResource CardBackgroundFillColorDefaultBrush}"
      BorderBrush="{ThemeResource CardStrokeColorDefaultBrush}">
    <Grid.ColumnDefinitions>
        <ColumnDefinition Width="*" />
        <ColumnDefinition Width="Auto" />
    </Grid.ColumnDefinitions>
    <StackPanel VerticalAlignment="Center">
        <TextBlock x:Name="Title" TextWrapping="Wrap" />
        <TextBlock x:Name="Description" TextWrapping="Wrap" FontSize="12"
                   Foreground="{ThemeResource TextFillColorSecondaryBrush}" />
    </StackPanel>
</Grid>)").as<Grid>();
        grid.FindName(L"Title").as<TextBlock>().Text(title);
        auto const descriptionBlock = grid.FindName(L"Description").as<TextBlock>();
        descriptionBlock.Text(description);
        descriptionBlock.Visibility(description.empty() ? Visibility::Collapsed : Visibility::Visible);

        auto const element = control.as<FrameworkElement>();
        element.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(element, 1);
        grid.Children().Append(control);
        return grid;
    }

    ToggleSwitch MakeToggle(bool on, std::function<bool(bool)> apply)
    {
        ToggleSwitch toggle;
        toggle.OnContent(box_value(Str(L"Settings", L"On")));
        toggle.OffContent(box_value(Str(L"Settings", L"Off")));
        toggle.IsOn(on);
        auto const reverting = std::make_shared<bool>(false);
        toggle.Toggled([apply = std::move(apply), reverting](Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
            if (*reverting)
            {
                return;
            }
            auto const self = sender.as<ToggleSwitch>();
            if (!apply(self.IsOn()))
            {
                *reverting = true;
                self.IsOn(!self.IsOn());
                *reverting = false;
            }
        });
        return toggle;
    }

    ComboBox MakeCombo(std::initializer_list<hstring> items, int selected, std::function<bool(int)> apply)
    {
        ComboBox combo;
        combo.MinWidth(180);
        for (auto const& item : items)
        {
            combo.Items().Append(box_value(item));
        }
        combo.SelectedIndex(selected);
        struct State
        {
            int current;
            bool reverting = false;
        };
        auto const state = std::make_shared<State>(State{selected});
        combo.SelectionChanged([apply = std::move(apply), state](Windows::Foundation::IInspectable const& sender, SelectionChangedEventArgs const&) {
            auto const self = sender.as<ComboBox>();
            int const index = self.SelectedIndex();
            if (state->reverting || index < 0)
            {
                return;
            }
            if (apply(index))
            {
                state->current = index;
            }
            else
            {
                state->reverting = true;
                self.SelectedIndex(state->current);
                state->reverting = false;
            }
        });
        return combo;
    }

    InfoBar MakeErrorBar()
    {
        InfoBar bar;
        bar.Severity(InfoBarSeverity::Error);
        bar.IsOpen(false);
        return bar;
    }

    void ShowError(WeakBar const& weak, std::wstring_view key)
    {
        if (auto const bar = weak.get())
        {
            bar.Severity(InfoBarSeverity::Error);
            bar.Title(Str(L"Settings", key));
            bar.IsOpen(true);
        }
    }

    void ShowSaveResult(WeakBar const& weak, bool saved)
    {
        if (saved)
        {
            if (auto const bar = weak.get())
            {
                bar.IsOpen(false);
            }
        }
        else
        {
            ShowError(weak, L"SaveFailed");
        }
    }

    hstring FormatOne(hstring const& pattern, std::wstring const& value)
    {
        std::wstring text{pattern};
        if (auto const at = text.find(L"{0}"); at != std::wstring::npos)
        {
            text.replace(at, 3, value);
        }
        return hstring{text};
    }
}
