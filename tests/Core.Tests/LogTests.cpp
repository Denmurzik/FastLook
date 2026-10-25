#include <regex>

#include "Diagnostics/Log.h"
#include "Diagnostics/PerfLog.h"
#include "TestUtil.h"

using namespace fastlook::core;

TEST(Log, RotatesAtLimitKeepingTwoFiles)
{
    auto dir = MakeTempDir();
    log::Init(dir, L"t", LogLevel::Info, 4096);
    for (int i = 0; i < 100; ++i)
        log::Write(LogLevel::Info, std::wstring(100, L'x'));
    EXPECT_TRUE(std::filesystem::exists(dir / "t.log"));
    EXPECT_TRUE(std::filesystem::exists(dir / "t.1.log"));
    EXPECT_FALSE(std::filesystem::exists(dir / "t.2.log"));
    EXPECT_LE(std::filesystem::file_size(dir / "t.log"), 4096u);
    EXPECT_LE(std::filesystem::file_size(dir / "t.1.log"), 4096u);
}

TEST(Log, LineFormat)
{
    auto dir = MakeTempDir();
    log::Init(dir, L"fmt", LogLevel::Info);
    log::Write(LogLevel::Info, L"abc");
    auto text = ReadFile(dir / "fmt.log");
    EXPECT_TRUE(std::regex_match(text, std::regex(R"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3} \[I\] \[\d+\] abc\r\n)"))) << text;
}

TEST(Log, WritesUtf8)
{
    auto dir = MakeTempDir();
    log::Init(dir, L"u8", LogLevel::Info);
    log::Write(LogLevel::Info, L"\u044F");
    EXPECT_NE(ReadFile(dir / "u8.log").find("\xD1\x8F"), std::string::npos);
}

TEST(Log, InfoRedactsPathsDebugShowsThem)
{
    log::Init(MakeTempDir(), L"redact", LogLevel::Info);
    log::SetLevel(LogLevel::Info);
    EXPECT_EQ(log::RedactPath(L"C:\\s\\a.txt"), L"<path>");
    log::SetLevel(LogLevel::Debug);
    EXPECT_EQ(log::RedactPath(L"C:\\s\\a.txt"), L"C:\\s\\a.txt");
}

TEST(Log, DebugLinesDroppedAtInfo)
{
    auto dir = MakeTempDir();
    log::Init(dir, L"drop", LogLevel::Info);
    log::Write(LogLevel::Debug, L"zzz");
    log::Write(LogLevel::Info, L"kept");
    auto text = ReadFile(dir / "drop.log");
    EXPECT_EQ(text.find("zzz"), std::string::npos);
    EXPECT_NE(text.find("kept"), std::string::npos);
    log::SetLevel(LogLevel::Debug);
    log::Write(LogLevel::Debug, L"visible");
    EXPECT_NE(ReadFile(dir / "drop.log").find("[D]"), std::string::npos);
}

TEST(PerfLog, MarkWritesQpcAndName)
{
    auto f = MakeTempDir() / "perf.log";
    perf::Mark("before_open");
    perf::Open(f);
    EXPECT_TRUE(perf::Enabled());
    perf::Mark("window_visible", "kind=image");
    auto text = ReadFile(f);
    EXPECT_EQ(text.rfind("window_visible qpc=", 0), 0u) << text;
    EXPECT_NE(text.find(" freq="), std::string::npos);
    EXPECT_NE(text.find(" ft="), std::string::npos);
    EXPECT_NE(text.find("kind=image"), std::string::npos);
    EXPECT_EQ(text.find("before_open"), std::string::npos);
}
