#include <gtest/gtest.h>

#include <windows.h>

#include <chrono>
#include <string>
#include <thread>

#include "Ipc/Pipe.h"

using namespace fastlook::core;
using namespace fastlook::core::ipc;
using namespace std::chrono_literals;

namespace
{
    std::wstring TestPipeName(wchar_t const* tag)
    {
        return std::wstring(L"\\\\.\\pipe\\FastLook-test-") + tag + L"-" + std::to_wstring(GetCurrentProcessId());
    }

    long long ElapsedMs(std::chrono::steady_clock::time_point since)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
    }

    // Connects a client to `srv` and returns {client, server side}.
    std::pair<std::optional<PipeConnection>, std::optional<PipeConnection>> Pair(PipeServer& srv, std::wstring const& name)
    {
        std::optional<PipeConnection> serverSide;
        std::thread t([&] { serverSide = srv.Accept(3000ms); });
        auto cli = ConnectPipe(name, 3000ms);
        t.join();
        return {std::move(cli), std::move(serverSide)};
    }
}

TEST(IpcPipe, PingPongRoundTrip)
{
    auto name = TestPipeName(L"pingpong");
    PipeServer srv(name);
    std::thread t([&] {
        auto c = srv.Accept(2000ms);
        ASSERT_TRUE(c);
        auto m = c->Receive(2000ms);
        ASSERT_TRUE(m);
        c->Send({.type = MsgType::Pong, .id = 2, .replyTo = m->id}, 1000ms);
    });
    auto cli = ConnectPipe(name, 2000ms);
    ASSERT_TRUE(cli);
    ASSERT_TRUE(cli->Send({.type = MsgType::Ping, .id = 1}, 1000ms));
    auto r = cli->Receive(2000ms);
    t.join();
    ASSERT_TRUE(r);
    EXPECT_EQ(r->type, MsgType::Pong);
    EXPECT_EQ(r->replyTo, 1u);
}

TEST(IpcPipe, LargeMessageSurvivesChunking)
{
    auto name = TestPipeName(L"large");
    PipeServer srv(name);
    Message big{.type = MsgType::Items, .id = 3};
    for (int i = 0; i < 20000; ++i)
    {
        big.items.items.push_back({L"C:\\folder\\Подпапка\\file-" + std::to_wstring(i) + L".txt"});
    }
    std::thread t([&] {
        auto c = srv.Accept(2000ms);
        ASSERT_TRUE(c);
        EXPECT_TRUE(c->Send(big, 5000ms));
        c->Receive(2000ms); // keep the connection open until the client is done
    });
    auto cli = ConnectPipe(name, 2000ms);
    ASSERT_TRUE(cli);
    auto r = cli->Receive(5000ms);
    cli->Close();
    t.join();
    ASSERT_TRUE(r);
    EXPECT_EQ(*r, big);
}

TEST(IpcPipe, ReceiveTimesOut)
{
    auto name = TestPipeName(L"timeout");
    PipeServer srv(name);
    auto [cli, serverSide] = Pair(srv, name);
    ASSERT_TRUE(cli);
    ASSERT_TRUE(serverSide);

    auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(cli->Receive(100ms));
    auto ms = ElapsedMs(start);
    EXPECT_GE(ms, 90);
    EXPECT_LE(ms, 600);
    EXPECT_TRUE(cli->Connected());
}

TEST(IpcPipe, ReceiveSeesDisconnect)
{
    auto name = TestPipeName(L"disconnect");
    PipeServer srv(name);
    auto [cli, serverSide] = Pair(srv, name);
    ASSERT_TRUE(cli);
    ASSERT_TRUE(serverSide);

    serverSide->Close();
    EXPECT_FALSE(cli->Receive(1000ms));
    EXPECT_FALSE(cli->Connected());
    EXPECT_FALSE(cli->Send({.type = MsgType::Ping, .id = 1}, 100ms));
}

TEST(IpcPipe, CloseUnblocksReceiveOnAnotherThread)
{
    auto name = TestPipeName(L"closeunblock");
    PipeServer srv(name);
    auto [cli, serverSide] = Pair(srv, name);
    ASSERT_TRUE(cli);
    ASSERT_TRUE(serverSide);

    auto start = std::chrono::steady_clock::now();
    std::thread closer([&] {
        std::this_thread::sleep_for(100ms);
        cli->Close();
    });
    EXPECT_FALSE(cli->Receive(5000ms));
    closer.join();
    EXPECT_LT(ElapsedMs(start), 2000);
    EXPECT_FALSE(cli->Connected());
}

TEST(IpcPipe, SecondClientIsAcceptedAfterFirstCloses)
{
    auto name = TestPipeName(L"reaccept");
    PipeServer srv(name);
    for (int round = 0; round < 2; ++round)
    {
        auto [cli, serverSide] = Pair(srv, name);
        ASSERT_TRUE(cli) << round;
        ASSERT_TRUE(serverSide) << round;
        ASSERT_TRUE(cli->Send({.type = MsgType::Ping, .id = 1}, 1000ms));
        EXPECT_TRUE(serverSide->Receive(1000ms));
    }
}

TEST(IpcPipe, CancelUnblocksAccept)
{
    PipeServer srv(TestPipeName(L"cancel"));
    auto start = std::chrono::steady_clock::now();
    std::thread t([&] {
        std::this_thread::sleep_for(100ms);
        srv.Cancel();
    });
    EXPECT_FALSE(srv.Accept(5000ms));
    t.join();
    EXPECT_LT(ElapsedMs(start), 2000);
    EXPECT_FALSE(srv.Accept(100ms));
}

TEST(IpcPipe, AcceptTimesOut)
{
    PipeServer srv(TestPipeName(L"accepttimeout"));
    auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(srv.Accept(100ms));
    auto ms = ElapsedMs(start);
    EXPECT_GE(ms, 90);
    EXPECT_LE(ms, 600);
}

TEST(IpcPipe, ConnectFailsFastWithoutServer)
{
    auto start = std::chrono::steady_clock::now();
    auto c = ConnectPipe(TestPipeName(L"nobody"), 200ms);
    EXPECT_FALSE(c);
    EXPECT_LT(ElapsedMs(start), 1000);
}

TEST(IpcPipe, NameHasSidAndVersion)
{
    auto n = PipeNameForCurrentUser();
    EXPECT_TRUE(n.starts_with(L"\\\\.\\pipe\\FastLook-S-1-5-"));
    EXPECT_TRUE(n.ends_with(L"-v1"));
}
