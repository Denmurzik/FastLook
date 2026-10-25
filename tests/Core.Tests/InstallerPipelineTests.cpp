#include "TestUtil.h"

#include <chrono>
#include <map>
#include <thread>

#include "Crypto/Sha256.h"
#include "Update/InstallerPipeline.h"

using namespace fastlook::core;

namespace
{
    // The "server": url -> local file. Copying stands in for the WinHTTP download.
    struct FakeServer
    {
        std::map<std::string, std::filesystem::path> files;
        std::stop_source* stopOnSetup = nullptr; // simulates the user cancelling while the installer is being hashed
        std::vector<std::string> requested;

        FetchFile Fetch()
        {
            return [this](std::string const& url, std::filesystem::path const& to, uint64_t maxBytes, ProgressFn const&,
                          std::stop_token) {
                requested.push_back(url);
                auto it = files.find(url);
                std::error_code ec;
                if (it == files.end() || std::filesystem::file_size(it->second, ec) > maxBytes)
                {
                    return false;
                }
                std::filesystem::copy_file(it->second, to, std::filesystem::copy_options::overwrite_existing, ec);
                if (!ec && stopOnSetup && url.ends_with("setup.cmd"))
                {
                    stopOnSetup->request_stop();
                }
                return !ec;
            };
        }
    };

    struct Scenario
    {
        std::filesystem::path root = MakeTempDir();
        std::filesystem::path served = root / L"served";
        std::filesystem::path work = root / L"work" / L"FastLook-Update";
        ReleaseInfo release;
        FakeServer server;

        // The dummy installer records its command line next to itself and exits.
        explicit Scenario(bool goodHash)
        {
            std::filesystem::create_directories(served);
            std::ofstream(served / L"setup.cmd", std::ios::binary) << "@echo %*>\"%~dp0ran.txt\"\r\n";
            auto digest = Sha256File(served / L"setup.cmd");
            auto hex = ToHex(*digest);
            if (!goodHash)
            {
                hex[0] = hex[0] == '0' ? '1' : '0';
            }
            std::ofstream(served / L"setup.cmd.sha256", std::ios::binary) << hex << "  setup.cmd\n";
            release.setupName = "setup.cmd";
            release.setupUrl = "https://github.com/x/setup.cmd";
            release.shaUrl = "https://github.com/x/setup.cmd.sha256";
            server.files[release.setupUrl] = served / L"setup.cmd";
            server.files[release.shaUrl] = served / L"setup.cmd.sha256";
        }
    };
}

TEST(InstallerPipeline, VerifiedInstallerIsLaunchedWithSilentArguments)
{
    Scenario s(true);
    auto outcome = DownloadAndVerifyInstaller(s.release, s.work, s.server.Fetch(), nullptr, std::stop_token{});
    ASSERT_EQ(outcome, DownloadOutcome::Verified);
    ASSERT_TRUE(std::filesystem::exists(s.work / L"setup.cmd"));

    ASSERT_TRUE(LaunchInstaller(s.work / L"setup.cmd"));
    auto const marker = s.work / L"ran.txt";
    for (int i = 0; i < 100 && !std::filesystem::exists(marker); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(std::filesystem::exists(marker));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto const text = ReadFile(marker);
    EXPECT_NE(text.find("/SILENT /SUPPRESSMSGBOXES /NORESTART"), std::string::npos) << text;
}

TEST(InstallerPipeline, HashMismatchDeletesBothFiles)
{
    Scenario s(false);
    auto outcome = DownloadAndVerifyInstaller(s.release, s.work, s.server.Fetch(), nullptr, std::stop_token{});
    EXPECT_EQ(outcome, DownloadOutcome::Mismatch);
    EXPECT_FALSE(std::filesystem::exists(s.work / L"setup.cmd"));
    EXPECT_FALSE(std::filesystem::exists(s.work / L"setup.cmd.sha256"));
}

TEST(InstallerPipeline, CancelWhileHashingNeverVerifies)
{
    Scenario s(true);
    std::stop_source source;
    s.server.stopOnSetup = &source;
    auto outcome = DownloadAndVerifyInstaller(s.release, s.work, s.server.Fetch(), nullptr, source.get_token());
    EXPECT_EQ(outcome, DownloadOutcome::Cancelled);
    EXPECT_FALSE(std::filesystem::exists(s.work / L"setup.cmd"));
}

TEST(InstallerPipeline, FetchFailureAndMissingChecksum)
{
    Scenario s(true);
    s.server.files.erase(s.release.shaUrl);
    EXPECT_EQ(DownloadAndVerifyInstaller(s.release, s.work, s.server.Fetch(), nullptr, std::stop_token{}), DownloadOutcome::Failed);
    EXPECT_FALSE(std::filesystem::exists(s.work / L"setup.cmd"));
}

TEST(InstallerPipeline, RefusesRelativeDirectory)
{
    Scenario s(true);
    EXPECT_EQ(DownloadAndVerifyInstaller(s.release, std::filesystem::path{}, s.server.Fetch(), nullptr, std::stop_token{}),
              DownloadOutcome::Failed);
    EXPECT_EQ(DownloadAndVerifyInstaller(s.release, L"relative-dir", s.server.Fetch(), nullptr, std::stop_token{}),
              DownloadOutcome::Failed);
    EXPECT_TRUE(s.server.requested.empty());
}

TEST(InstallerPipeline, OversizedChecksumRejected)
{
    Scenario s(true);
    std::ofstream(s.served / L"setup.cmd.sha256", std::ios::binary | std::ios::trunc) << std::string(2048, 'a');
    EXPECT_EQ(DownloadAndVerifyInstaller(s.release, s.work, s.server.Fetch(), nullptr, std::stop_token{}), DownloadOutcome::Failed);
}
