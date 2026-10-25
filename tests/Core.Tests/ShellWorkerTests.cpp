#include <Windows.h>
#include <objbase.h>

#include <chrono>

#include "Shell/ShellWorker.h"
#include "TestUtil.h"

using namespace fastlook::core;
using namespace std::chrono_literals;

TEST(ShellWorker, RunsOnStaThread)
{
    ShellWorker w;
    auto r = w.Run(
        [] {
            APTTYPE t = APTTYPE_CURRENT;
            APTTYPEQUALIFIER q;
            CoGetApartmentType(&t, &q);
            return t;
        },
        1000ms);
    ASSERT_TRUE(r);
    EXPECT_TRUE(*r == APTTYPE_STA || *r == APTTYPE_MAINSTA); // the first STA thread of a process is the main STA
}

TEST(ShellWorker, ReusesTheSameThreadBetweenCalls)
{
    ShellWorker w;
    auto a = w.Run([] { return GetCurrentThreadId(); }, 1000ms);
    auto b = w.Run([] { return GetCurrentThreadId(); }, 1000ms);
    ASSERT_TRUE(a && b);
    EXPECT_EQ(*a, *b);
    EXPECT_NE(*a, GetCurrentThreadId());
}

TEST(ShellWorker, ThrowingJobYieldsNulloptAndWorkerSurvives)
{
    ShellWorker w;
    EXPECT_FALSE(w.Run([]() -> int { throw 1; }, 1000ms));
    EXPECT_EQ(w.Run([] { return 5; }, 1000ms), 5);
    EXPECT_EQ(w.AbandonedCount(), 0u);
}

TEST(ShellWorker, TimeoutAbandonsAndRecovers)
{
    ShellWorker w;
    auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(w.Run([] { Sleep(3000); return 1; }, 1000ms));
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 1500ms);
    EXPECT_EQ(w.AbandonedCount(), 1u);
    EXPECT_EQ(w.Run([] { return 2; }, 1000ms), 2); // fresh thread works immediately
}

TEST(ShellWorker, DestructionWithAbandonedThreadIsSafe)
{
    {
        ShellWorker w;
        w.Run([] { Sleep(1500); return 0; }, 100ms);
    }
    Sleep(2000);
    SUCCEED();
}

TEST(ShellWorker, AbandonedCapFailsFast)
{
    ShellWorker w;
    for (int i = 0; i < 3; ++i)
        EXPECT_FALSE(w.Run([] { Sleep(3000); return 0; }, 100ms));
    EXPECT_EQ(w.AbandonedCount(), 3u);

    auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(w.Run([] { return 1; }, 1000ms));
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 50ms);

    Sleep(3500);
    EXPECT_EQ(w.AbandonedCount(), 0u);
    EXPECT_EQ(w.Run([] { return 2; }, 1000ms), 2);
}
