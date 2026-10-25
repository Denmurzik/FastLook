#include "pch.h"
#include "Update/UpdateFlow.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stop_token>

#include "Common/Version.h"
#include "Crypto/Sha256.h"
#include "Diagnostics/Log.h"
#include "System/AppPaths.h"
#include "Text/Utf.h"
#include "Update/Http.h"
#include "Update/ReleaseInfo.h"
#include "Util/Async.h"
#include "Util/SettingsUi.h"
#include "Util/Str.h"

namespace fastlook::viewer
{
    namespace
    {
        using namespace winrt;
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        using Microsoft::UI::Dispatching::DispatcherQueue;
        using Windows::Foundation::IAsyncOperation;

        constexpr wchar_t kMap[] = L"Update";
        constexpr wchar_t kReleasesPage[] = L"https://github.com/Denmurzik/FastLook/releases/latest";
        constexpr wchar_t kInstallerArgs[] = L"/SILENT /SUPPRESSMSGBOXES /NORESTART";

        enum class FetchOutcome
        {
            Found,
            NoRelease,
            Failed
        };

        struct FetchResult
        {
            FetchOutcome outcome = FetchOutcome::Failed;
            core::ReleaseInfo release;
        };

        enum class DownloadOutcome
        {
            Verified,
            Cancelled,
            Failed,
            Mismatch
        };

        // A dialog that has been asked to show; Dismiss closes it even when it has not finished opening yet.
        struct ShownDialog
        {
            ContentDialog dialog{nullptr};
            IAsyncOperation<ContentDialogResult> operation{nullptr};
            std::shared_ptr<std::atomic<bool>> opened;
        };

        ShownDialog Show(XamlRoot const& root, hstring const& title, Windows::Foundation::IInspectable const& content,
                         hstring const& primary, hstring const& close)
        {
            ContentDialog dialog;
            dialog.XamlRoot(root);
            dialog.Title(box_value(title));
            dialog.Content(content);
            dialog.PrimaryButtonText(primary);
            dialog.CloseButtonText(close);
            dialog.DefaultButton(primary.empty() ? ContentDialogButton::Close : ContentDialogButton::Primary);
            ShownDialog shown;
            shown.dialog = dialog;
            shown.opened = std::make_shared<std::atomic<bool>>(false);
            dialog.Opened([opened = shown.opened](auto const&, auto const&) { *opened = true; });
            shown.operation = dialog.ShowAsync();
            return shown;
        }

        TextBlock WrappedText(hstring const& text)
        {
            TextBlock block;
            block.Text(text);
            block.TextWrapping(TextWrapping::Wrap);
            return block;
        }

        // Hides a dialog shown by Show and waits until it is gone. False when the UI queue is shutting down.
        IAsyncOperation<bool> Dismiss(ShownDialog shown, DispatcherQueue ui)
        {
            for (int i = 0; i < 100 && !*shown.opened && shown.operation.Status() == Windows::Foundation::AsyncStatus::Started; ++i)
            {
                co_await resume_after(std::chrono::milliseconds(20));
                if (!co_await ResumeOn(ui))
                {
                    co_return false;
                }
            }
            shown.dialog.Hide();
            try
            {
                co_await shown.operation;
            }
            catch (hresult_error const&)
            {
            }
            co_return true;
        }

        // Shows `content` with the given buttons and waits for the user's answer.
        IAsyncOperation<ContentDialogResult> Ask(XamlRoot root, hstring title, hstring text, hstring primary, hstring close)
        {
            auto shown = Show(root, title, WrappedText(text), primary, close);
            co_return co_await shown.operation;
        }

        std::filesystem::path UpdateDir()
        {
            wchar_t buffer[MAX_PATH + 2]{};
            DWORD const n = GetTempPathW(static_cast<DWORD>(std::size(buffer)), buffer);
            return std::filesystem::path(std::wstring(buffer, n)) / L"FastLook-Update";
        }

        std::optional<std::string> ReadWholeFile(std::filesystem::path const& path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                return std::nullopt;
            }
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        // Test hook: FASTLOOK_UPDATE_API names either a JSON file or a URL replacing the GitHub API address.
        std::wstring ApiOverride()
        {
            DWORD const len = GetEnvironmentVariableW(L"FASTLOOK_UPDATE_API", nullptr, 0);
            if (len <= 1)
            {
                return {};
            }
            std::wstring value(len, L'\0');
            value.resize(GetEnvironmentVariableW(L"FASTLOOK_UPDATE_API", value.data(), len));
            return value;
        }

        // Runs on a worker thread.
        FetchResult FetchLatest()
        {
            FetchResult result;
            std::string body;
            auto const override = ApiOverride();
            std::error_code ec;
            if (!override.empty() && std::filesystem::is_regular_file(override, ec))
            {
                auto text = ReadWholeFile(override);
                if (!text)
                {
                    return result;
                }
                body = std::move(*text);
            }
            else
            {
                auto const url = override.empty() ? core::FromUtf8(core::kReleasesApi) : override;
                auto const response = HttpGet(url, std::chrono::seconds(15), !override.empty());
                if (!response)
                {
                    return result;
                }
                if (response->status == 404)
                {
                    result.outcome = FetchOutcome::NoRelease;
                    return result;
                }
                if (response->status != 200)
                {
                    return result;
                }
                body.assign(response->body.begin(), response->body.end());
            }
            auto release = core::ParseLatestRelease(body);
            if (!release)
            {
                FL_LOG_INFO(L"Update: the latest release document has no usable installer assets");
                return result;
            }
            result.outcome = FetchOutcome::Found;
            result.release = std::move(*release);
            return result;
        }

        // Runs on a worker thread. Leaves only a verified installer in `dir`.
        DownloadOutcome DownloadAndVerify(core::ReleaseInfo const& release, std::filesystem::path const& dir,
                                          std::function<void(uint64_t, uint64_t)> const& progress, std::stop_token stop)
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
            std::filesystem::create_directories(dir, ec);
            auto const setup = dir / release.setupName;
            auto const sha = dir / (release.setupName + ".sha256");
            auto const fail = [&](DownloadOutcome outcome) {
                std::error_code ignore;
                std::filesystem::remove(setup, ignore);
                std::filesystem::remove(sha, ignore);
                return outcome;
            };

            if (!HttpDownload(core::FromUtf8(release.shaUrl), sha, nullptr, stop))
            {
                return fail(stop.stop_requested() ? DownloadOutcome::Cancelled : DownloadOutcome::Failed);
            }
            if (!HttpDownload(core::FromUtf8(release.setupUrl), setup, progress, stop))
            {
                return fail(stop.stop_requested() ? DownloadOutcome::Cancelled : DownloadOutcome::Failed);
            }
            auto const shaText = ReadWholeFile(sha);
            auto const expected = shaText ? core::ParseSha256Line(*shaText, release.setupName) : std::nullopt;
            auto const actual = core::Sha256File(setup);
            if (!expected || !actual || *expected != *actual)
            {
                FL_LOG_INFO(L"Update: installer SHA-256 check failed");
                return fail(DownloadOutcome::Mismatch);
            }
            return DownloadOutcome::Verified;
        }

        bool LaunchInstaller(std::filesystem::path const& setup)
        {
            SHELLEXECUTEINFOW info{};
            info.cbSize = sizeof(info);
            info.lpVerb = L"open";
            auto const file = setup.wstring();
            auto const dir = setup.parent_path().wstring();
            info.lpFile = file.c_str();
            info.lpParameters = kInstallerArgs;
            info.lpDirectory = dir.c_str();
            info.nShow = SW_SHOWNORMAL;
            return ShellExecuteExW(&info) != FALSE;
        }

        void OpenReleasePage(core::ReleaseInfo const& release)
        {
            auto const url = release.htmlUrl.empty() ? std::wstring{kReleasesPage} : core::FromUtf8(release.htmlUrl);
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }

        // Downloads, verifies and starts the installer, then ends this process. Reports every failure in a dialog.
        Windows::Foundation::IAsyncAction DownloadAndInstall(XamlRoot root, DispatcherQueue ui, core::ReleaseInfo release)
        {
            auto const title = Str(kMap, L"Title");
            auto const downloadingPattern = Str(kMap, L"Downloading");

            ProgressBar bar;
            bar.IsIndeterminate(true);
            bar.Minimum(0);
            bar.Maximum(100);
            TextBlock status = WrappedText(FormatOne(downloadingPattern, L"0"));
            StackPanel content;
            content.Spacing(12);
            content.Children().Append(status);
            content.Children().Append(bar);

            std::stop_source cancel;
            auto shown = Show(root, title, content, {}, Str(kMap, L"Cancel"));
            // Only the stop source is captured: the dialog owns the button, so a capture of a control would be a cycle.
            shown.dialog.CloseButtonClick([source = cancel](auto const&, auto const&) mutable { source.request_stop(); });

            auto const progress = [ui, weakBar = make_weak(bar), weakStatus = make_weak(status), downloadingPattern,
                                   last = std::make_shared<std::atomic<int>>(-1)](uint64_t got, uint64_t total) {
                if (total == 0)
                {
                    return;
                }
                int const percent = static_cast<int>(got * 100 / total);
                if (last->exchange(percent) == percent)
                {
                    return;
                }
                ui.TryEnqueue([=] {
                    if (auto b = weakBar.get())
                    {
                        b.IsIndeterminate(false);
                        b.Value(percent);
                    }
                    if (auto s = weakStatus.get())
                    {
                        s.Text(FormatOne(downloadingPattern, std::to_wstring(percent)));
                    }
                });
            };

            auto const dir = UpdateDir();
            co_await resume_background();
            auto const outcome = DownloadAndVerify(release, dir, progress, cancel.get_token());
            if (!co_await ResumeOn(ui))
            {
                co_return;
            }
            if (!co_await Dismiss(shown, ui))
            {
                co_return;
            }

            switch (outcome)
            {
            case DownloadOutcome::Cancelled:
                co_return;
            case DownloadOutcome::Mismatch:
                co_await Ask(root, title, Str(kMap, L"HashMismatch"), {}, Str(kMap, L"Close"));
                co_return;
            case DownloadOutcome::Failed:
                co_await Ask(root, title, Str(kMap, L"NetworkError"), {}, Str(kMap, L"Close"));
                co_return;
            case DownloadOutcome::Verified:
                break;
            }

            if (!LaunchInstaller(dir / release.setupName))
            {
                FL_LOG_INFO(L"Update: starting the installer failed ({})", GetLastError());
                co_await Ask(root, title, Str(kMap, L"InstallFailed"), {}, Str(kMap, L"Close"));
                co_return;
            }
            // The installer closes FastLook (`FastLook.exe --shutdown`) itself; this process just gets out of its way.
            Application::Current().Exit();
        }

        Windows::Foundation::IAsyncAction RunCheck(XamlRoot root)
        {
            auto const ui = DispatcherQueue::GetForCurrentThread();
            auto const title = Str(kMap, L"Title");

            ProgressBar busy;
            busy.IsIndeterminate(true);
            StackPanel checking;
            checking.Spacing(12);
            checking.Children().Append(WrappedText(Str(kMap, L"Checking")));
            checking.Children().Append(busy);

            std::stop_source cancel;
            auto shown = Show(root, title, checking, {}, Str(kMap, L"Cancel"));
            shown.dialog.CloseButtonClick([source = cancel](auto const&, auto const&) mutable { source.request_stop(); });

            co_await resume_background();
            auto fetched = FetchLatest();
            if (!co_await ResumeOn(ui))
            {
                co_return;
            }
            if (!co_await Dismiss(shown, ui) || cancel.stop_requested())
            {
                co_return;
            }

            auto const close = Str(kMap, L"Close");
            if (fetched.outcome == FetchOutcome::NoRelease)
            {
                co_await Ask(root, title, Str(kMap, L"NoReleases"), {}, close);
                co_return;
            }
            if (fetched.outcome != FetchOutcome::Found)
            {
                co_await Ask(root, title, Str(kMap, L"NetworkError"), {}, close);
                co_return;
            }

            auto const& release = fetched.release;
            auto const current = core::ParseSemVer(FASTLOOK_VERSION_STR);
            if (current && core::CompareSemVer(*current, release.version) >= 0)
            {
                co_await Ask(root, title, Str(kMap, L"UpToDate"), {}, close);
                co_return;
            }

            auto const bare = release.tag.starts_with('v') ? release.tag.substr(1) : release.tag;
            auto const heading = FormatOne(Str(kMap, L"NewVersion"), core::FromUtf8(bare));
            bool const portable = core::CurrentAppPaths().portable;
            auto const primary = Str(kMap, portable ? L"OpenReleasePage" : L"Download");
            if (co_await Ask(root, title, heading, primary, close) != ContentDialogResult::Primary)
            {
                co_return;
            }
            if (portable)
            {
                OpenReleasePage(release);
                co_return;
            }
            co_await DownloadAndInstall(root, ui, release);
        }
    }

    winrt::Windows::Foundation::IAsyncAction CheckForUpdatesInteractive(winrt::Microsoft::UI::Xaml::XamlRoot root)
    {
        try
        {
            co_await RunCheck(root);
        }
        catch (winrt::hresult_error const& e)
        {
            FL_LOG_INFO(L"Update flow failed: 0x{:08X}", static_cast<uint32_t>(e.code().value));
        }
        catch (std::exception const&)
        {
            FL_LOG_INFO(L"Update flow failed");
        }
    }
}
