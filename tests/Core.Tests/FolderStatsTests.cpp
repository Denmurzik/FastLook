#include <Windows.h>

#include <chrono>
#include <cstdlib>
#include <stop_token>

#include "Fs/FolderStats.h"
#include "TestUtil.h"

using namespace fastlook::core;
using namespace std::chrono;

namespace
{
    std::filesystem::path MakeTree()
    {
        auto const root = MakeTempDir();
        WriteFile(root / L"a.bin", std::string(10, 'a'));
        WriteFile(root / L"b.bin", std::string(20, 'b'));
        WriteFile(root / L"c.bin", std::string(30, 'c'));
        std::filesystem::create_directories(root / L"sub");
        WriteFile(root / L"sub" / L"d.bin", std::string(5, 'd'));
        WriteFile(root / L"sub" / L"e.bin", std::string(5, 'e'));
        return root;
    }

    auto Far() { return steady_clock::now() + seconds(30); }
}

TEST(FolderStats, CountsNestedTree)
{
    auto const stats = CountFolder(MakeTree(), {}, Far());
    EXPECT_EQ(stats.files, 5u);
    EXPECT_EQ(stats.folders, 1u);
    EXPECT_EQ(stats.bytes, 70u);
    EXPECT_FALSE(stats.truncated);
}

TEST(FolderStats, JunctionLoopNotFollowed)
{
    auto const root = MakeTree();
    auto const loop = root / L"sub" / L"loop";
    auto const command = L"cmd /c mklink /J \"" + loop.wstring() + L"\" \"" + root.wstring() + L"\" >nul";
    if (_wsystem(command.c_str()) != 0)
    {
        GTEST_SKIP() << "cannot create a junction";
    }
    auto const stats = CountFolder(root, {}, Far());
    EXPECT_EQ(stats.files, 5u);
    EXPECT_EQ(stats.folders, 2u); // sub + the junction, counted once
    EXPECT_EQ(stats.bytes, 70u);
    EXPECT_FALSE(stats.truncated);
    RemoveDirectoryW(loop.c_str()); // remove the link itself before temp cleanup
}

TEST(FolderStats, DeadlineTruncates)
{
    auto const stats = CountFolder(MakeTree(), {}, steady_clock::now());
    EXPECT_TRUE(stats.truncated);
}

TEST(FolderStats, StopTokenCancels)
{
    std::stop_source source;
    source.request_stop();
    auto const stats = CountFolder(MakeTree(), source.get_token(), Far());
    EXPECT_TRUE(stats.truncated);
}

TEST(FolderStats, ProgressReportsFinalValue)
{
    FolderStats final;
    CountFolder(MakeTree(), {}, Far(), [&](FolderStats const& s) { final = s; });
    EXPECT_EQ(final.files, 5u);
}
