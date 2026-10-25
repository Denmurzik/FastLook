#include "pch.h"
#include "SettingsAboutPage.xaml.h"
#if __has_include("SettingsAboutPage.g.cpp")
#include "SettingsAboutPage.g.cpp"
#endif

#include <filesystem>

#include <winrt/Microsoft.UI.Text.h>

#include "Common/Version.h"
#include "Diagnostics/Log.h"
#include "System/AppPaths.h"
#include "Update/UpdateFlow.h"
#include "Util/SettingsUi.h"
#include "Util/Str.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace fastlook;
using namespace fastlook::viewer;

namespace
{
    constexpr wchar_t kRepositoryUrl[] = L"https://github.com/Denmurzik/FastLook";
    constexpr wchar_t kNoticesFile[] = L"THIRD_PARTY_NOTICES.txt";

    void Open(std::wstring const& path, fastlook::viewer::WeakBar const& error)
    {
        std::error_code ec;
        bool opened = false;
        if (std::filesystem::exists(path, ec))
        {
            // "open" on a folder shows it in Explorer, on the notices file in the default text viewer.
            opened = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
        }
        if (opened)
        {
            fastlook::viewer::ShowSaveResult(error, true);
        }
        else
        {
            FL_LOG_INFO(L"Opening a settings link failed");
            fastlook::viewer::ShowError(error, L"OpenFailed");
        }
    }

    // Parameters are by value: they are used after the first co_await.
    fire_and_forget CheckUpdates(Button button, XamlRoot root)
    {
        button.IsEnabled(false);
        try
        {
            co_await fastlook::viewer::CheckForUpdatesInteractive(root);
        }
        catch (...)
        {
            FL_LOG_INFO(L"Update check from settings failed");
        }
        button.IsEnabled(true);
    }

    Button MakeButton(hstring const& text)
    {
        Button button;
        button.Content(box_value(text));
        return button;
    }
}

namespace winrt::FastLook::implementation
{
    SettingsAboutPage::SettingsAboutPage()
    {
        InitializeComponent();
        Build();
    }

    void SettingsAboutPage::Build()
    {
        auto rows = Rows().Children();
        m_error = MakeErrorBar();
        rows.Append(m_error);
        WeakBar const error = make_weak(m_error);

        StackPanel header;
        header.Spacing(2);
        header.Margin(ThicknessHelper::FromLengths(0, 8, 0, 16));
        TextBlock name;
        name.Text(Str(L"Settings", L"AppName"));
        name.FontSize(28);
        name.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
        header.Children().Append(name);
        TextBlock tagline;
        tagline.Text(Str(L"Settings", L"AppTagline"));
        header.Children().Append(tagline);
        TextBlock version;
        version.Text(FormatOne(Str(L"Settings", L"VersionLabel"), FASTLOOK_VERSION_WSTR));
        version.Opacity(0.7);
        header.Children().Append(version);
        rows.Append(header);

        auto check = MakeButton(Str(L"Settings", L"CheckUpdates"));
        check.Click([](auto const& sender, auto const&) {
            auto const button = sender.template as<Button>();
            CheckUpdates(button, button.XamlRoot());
        });
        rows.Append(MakeSettingsRow(Str(L"Settings", L"CheckUpdates"), Str(L"Settings", L"CheckUpdatesDesc"), check));

        HyperlinkButton link;
        link.Content(box_value(hstring{kRepositoryUrl}));
        link.NavigateUri(Windows::Foundation::Uri{kRepositoryUrl});
        rows.Append(MakeSettingsRow(Str(L"Settings", L"GitHub"), {}, link));

        auto notices = MakeButton(Str(L"Settings", L"ThirdParty"));
        notices.Click([error](auto const&, auto const&) { Open((core::CurrentAppPaths().exeDir / kNoticesFile).wstring(), error); });
        rows.Append(MakeSettingsRow(Str(L"Settings", L"ThirdParty"), {}, notices));

        auto logs = MakeButton(Str(L"Settings", L"OpenLogs"));
        logs.Click([error](auto const&, auto const&) { Open(core::CurrentAppPaths().logsDir.wstring(), error); });
        rows.Append(MakeSettingsRow(Str(L"Settings", L"OpenLogs"), {}, logs));
    }
}
