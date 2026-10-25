#include "TestUtil.h"
#include "Cli/DaemonArgs.h"

using namespace fastlook::core;

TEST(DaemonArgs, Commands)
{
    std::wstring_view s[] = {L"--shutdown"};
    EXPECT_EQ(ParseDaemonArgs(s).command, DaemonCommand::Shutdown);
    std::wstring_view sh[] = {L"--show", L"C:\\a b.txt", L"--perf-log", L"p"};
    auto r = ParseDaemonArgs(sh);
    EXPECT_EQ(r.command, DaemonCommand::Show);
    EXPECT_EQ(r.showPath, L"C:\\a b.txt");
    EXPECT_EQ(r.perfLog, L"p");
    std::wstring_view w[] = {L"--warm"};
    EXPECT_TRUE(ParseDaemonArgs(w).warm);
    EXPECT_EQ(ParseDaemonArgs({}).command, DaemonCommand::Run);
    std::wstring_view t[] = {L"--toggle", L"C:\\x.txt"};
    auto tr = ParseDaemonArgs(t);
    EXPECT_EQ(tr.command, DaemonCommand::Toggle);
    EXPECT_EQ(tr.showPath, L"C:\\x.txt");
}

TEST(DaemonArgs, MissingValues)
{
    std::wstring_view a[] = {L"--show"};
    auto r = ParseDaemonArgs(a);
    EXPECT_EQ(r.command, DaemonCommand::Show);
    EXPECT_TRUE(r.showPath.empty());
    std::wstring_view b[] = {L"--perf-log"};
    EXPECT_TRUE(ParseDaemonArgs(b).perfLog.empty());
}
