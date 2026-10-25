#include "pch.h"
#include "SettingsPreviewPage.xaml.h"
#if __has_include("SettingsPreviewPage.g.cpp")
#include "SettingsPreviewPage.g.cpp"
#endif

#include <algorithm>
#include <cmath>

#include "Util/SettingsUi.h"
#include "Util/Str.h"
#include "ViewerHost.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace fastlook;
using namespace fastlook::viewer;

namespace
{
    constexpr double kMinFontSize = 8;
    constexpr double kMaxFontSize = 32;
}

namespace winrt::FastLook::implementation
{
    SettingsPreviewPage::SettingsPreviewPage()
    {
        InitializeComponent();
        Build();
    }

    void SettingsPreviewPage::Build()
    {
        auto const preview = ViewerHost::Get().Settings().preview;

        auto rows = Rows().Children();
        m_error = MakeErrorBar();
        rows.Append(m_error);
        WeakBar const error = make_weak(m_error);

        auto const toggleRow = [error](wchar_t const* key, wchar_t const* descriptionKey, bool on, bool core::PreviewSettings::*member) {
            return MakeSettingsRow(Str(L"Settings", key), Str(L"Settings", descriptionKey), MakeToggle(on, [error, member](bool value) {
                                       bool const ok = CommitSettings([member, value](core::Settings& s) { s.preview.*member = value; });
                                       ShowSaveResult(error, ok);
                                       return ok;
                                   }));
        };
        rows.Append(toggleRow(L"WordWrap", L"WordWrapDesc", preview.wordWrap, &core::PreviewSettings::wordWrap));

        NumberBox fontSize;
        fontSize.Minimum(kMinFontSize);
        fontSize.Maximum(kMaxFontSize);
        fontSize.SmallChange(1);
        fontSize.LargeChange(2);
        fontSize.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Inline);
        fontSize.ValidationMode(NumberBoxValidationMode::InvalidInputOverwritten);
        fontSize.Width(140);
        fontSize.Value(std::clamp<double>(preview.codeFontSize, kMinFontSize, kMaxFontSize));
        fontSize.ValueChanged([error](NumberBox const& box, NumberBoxValueChangedEventArgs const& args) {
            double const value = args.NewValue();
            if (std::isnan(value))
            {
                box.Value(args.OldValue()); // an emptied box keeps the previous size
                return;
            }
            auto const size = static_cast<uint32_t>(std::lround(std::clamp(value, kMinFontSize, kMaxFontSize)));
            bool const ok = CommitSettings([size](core::Settings& s) { s.preview.codeFontSize = size; });
            ShowSaveResult(error, ok);
            if (!ok && !std::isnan(args.OldValue()))
            {
                box.Value(args.OldValue()); // roll back; the re-entrant change saves the old size again
            }
        });
        rows.Append(MakeSettingsRow(Str(L"Settings", L"CodeFontSize"), Str(L"Settings", L"CodeFontSizeDesc"), fontSize));

        rows.Append(toggleRow(L"Autoplay", L"AutoplayDesc", preview.autoplayMedia, &core::PreviewSettings::autoplayMedia));
        rows.Append(toggleRow(L"ShellHandlers", L"ShellHandlersDesc", preview.useShellHandlers, &core::PreviewSettings::useShellHandlers));

        rows.Append(MakeSettingsRow(
            Str(L"Settings", L"MarkdownView"), Str(L"Settings", L"MarkdownViewDesc"),
            MakeCombo({Str(L"Settings", L"MarkdownRendered"), Str(L"Settings", L"MarkdownSource")},
                      preview.markdownView == core::MarkdownView::Source ? 1 : 0, [error](int index) {
                          auto const view = index == 1 ? core::MarkdownView::Source : core::MarkdownView::Rendered;
                          bool const ok = CommitSettings([view](core::Settings& s) { s.preview.markdownView = view; });
                          ShowSaveResult(error, ok);
                          return ok;
                      })));
    }
}
