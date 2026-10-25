#include "Update/InstallerPipeline.h"

#include <Windows.h>
#include <shellapi.h>

#include <fstream>
#include <iterator>

#include "Crypto/Sha256.h"
#include "Diagnostics/Log.h"

#pragma comment(lib, "shell32.lib")

namespace fastlook::core
{
    namespace
    {
        std::string ReadSmallFile(std::filesystem::path const& path)
        {
            std::ifstream in(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }

    DownloadOutcome DownloadAndVerifyInstaller(ReleaseInfo const& release, std::filesystem::path const& dir, FetchFile const& fetch,
                                               ProgressFn const& progress, std::stop_token stop)
    {
        // Never wipe a directory that is not an absolute path (an empty temp path must not turn into the CWD).
        if (!dir.is_absolute() || dir.filename().empty())
        {
            return DownloadOutcome::Failed;
        }
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
        auto const failed = [&] { return fail(stop.stop_requested() ? DownloadOutcome::Cancelled : DownloadOutcome::Failed); };

        if (!fetch(release.shaUrl, sha, kMaxChecksumBytes, nullptr, stop))
        {
            return failed();
        }
        if (!fetch(release.setupUrl, setup, kMaxInstallerBytes, progress, stop))
        {
            return failed();
        }
        auto const expected = ParseSha256Line(ReadSmallFile(sha), release.setupName);
        auto const actual = Sha256File(setup);
        if (stop.stop_requested())
        {
            return fail(DownloadOutcome::Cancelled);
        }
        if (!expected || !actual || *expected != *actual)
        {
            FL_LOG_INFO(L"Update: installer SHA-256 check failed");
            return fail(DownloadOutcome::Mismatch);
        }
        return DownloadOutcome::Verified;
    }

    bool LaunchInstaller(std::filesystem::path const& setup, int show)
    {
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.lpVerb = L"open";
        auto const file = setup.wstring();
        auto const dir = setup.parent_path().wstring();
        info.lpFile = file.c_str();
        info.lpParameters = kInstallerArgs;
        info.lpDirectory = dir.c_str();
        info.nShow = show;
        return ShellExecuteExW(&info) != FALSE;
    }
}
