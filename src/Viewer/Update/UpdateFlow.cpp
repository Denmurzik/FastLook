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
#include "Update/InstallerPipeline.h"
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

        // A dialog that has been asked to show; Dismiss closes it even when it has not finished opening yet.
        struct ShownDialog
        {
            ContentDialog dialog{nullptr};
            IAsyncOperation<ContentDialogResult> operation{nullptr};
            std::shared_ptr<std::atomic<bool>> opened;
            // Set by Dismiss before it hides the dialog: any other close is the user's (button, Esc, window gone).
            std::shared_ptr<std::atomic<bool>> dismissing;
        };

        ShownDialog Show(XamlRoot const& root, hstring const& title, Windows::Foundation::IInspectable const& content,
                         hstring const& primary, hstring const& close, std::stop_source cancel = {})
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
            shown.dismissing = std::make_shared<std::atomic<bool>>(false);
            dialog.Opened([opened = shown.opened](auto const&, auto const&) { *opened = true; });
            // Captures flags and the stop source only: the dialog owns this handler, a control capture would be a cycle.
            dialog.Closing([dismissing = shown.dismissing, source = cancel](auto const&, auto const&) mutable {
                if (!*dismissing)
                {
                    source.request_stop();
                }
            });
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
            *shown.dismissing = true;
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

        // Empty when the temp directory cannot be determined: callers must not wipe a relative path.
        std::filesystem::path UpdateDir()
        {
            std::wstring buffer(MAX_PATH + 2, wchar_t{});
            DWORD n = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
            if (n >= buffer.size())
            {
                buffer.resize(n + 1);
                n = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
            }
            if (n == 0 || n >= buffer.size())
            {
                return {};
            }
            buffer.resize(n);
            return std::filesystem::path(buffer) / L"FastLook-Update";
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
            if (!override.empty())
            {
                // Test hook: a local JSON file replaces the API answer. Anything else (a URL) is refused, not followed.
                auto text = std::filesystem::is_regular_file(override, ec) ? ReadWholeFile(override) : std::nullopt;
                if (!text)
                {
                    return result;
                }
                body = std::move(*text);
            }
            else
            {
                auto const response = HttpGet(core::FromUtf8(core::kReleasesApi), std::chrono::seconds(15));
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
            auto shown = Show(root, title, content, {}, Str(kMap, L"Cancel"), cancel);

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
            auto const fetch = [](std::string const& url, std::filesystem::path const& to, uint64_t maxBytes,
                                  core::ProgressFn const& onProgress, std::stop_token stop) {
                return HttpDownload(core::FromUtf8(url), to, onProgress, stop, maxBytes);
            };
            auto const outcome = core::DownloadAndVerifyInstaller(release, dir, fetch, progress, cancel.get_token());
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
            case core::DownloadOutcome::Cancelled:
                co_return;
            case core::DownloadOutcome::Mismatch:
                co_await Ask(root, title, Str(kMap, L"HashMismatch"), {}, Str(kMap, L"Close"));
                co_return;
            case core::DownloadOutcome::Failed:
                co_await Ask(root, title, Str(kMap, L"NetworkError"), {}, Str(kMap, L"Close"));
                co_return;
            case core::DownloadOutcome::Verified:
                break;
            }

            // The user may have closed the dialog while the installer was being hashed.
            if (cancel.stop_requested())
            {
                co_return;
            }
            auto installing = Show(root, title, WrappedText(Str(kMap, L"Installing")), {}, {});
            if (!core::LaunchInstaller(dir / release.setupName))
            {
                FL_LOG_INFO(L"Update: starting the installer failed ({})", GetLastError());
                if (!co_await Dismiss(installing, ui))
                {
                    co_return;
                }
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
            auto shown = Show(root, title, checking, {}, Str(kMap, L"Cancel"), cancel);

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
