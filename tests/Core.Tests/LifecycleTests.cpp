#include "Lifecycle/Lifecycle.h"
#include "TestUtil.h"

using namespace fastlook::core;

TEST(Lifecycle, TrimOneSecondAfterHide)
{
    Lifecycle l(0, 0);
    l.On(ViewerEvent::PreviewShown, 10);
    auto a = l.On(ViewerEvent::PreviewHidden, 100);
    EXPECT_TRUE(a.unloadContent);
    EXPECT_FALSE(l.On(ViewerEvent::Tick, 1099).trimNow);
    EXPECT_TRUE(l.On(ViewerEvent::Tick, 1100).trimNow);
    EXPECT_FALSE(l.On(ViewerEvent::Tick, 2000).trimNow);
}

TEST(Lifecycle, InitialTrimWaitsForPrewarmDone)
{
    Lifecycle l(0, 0);
    EXPECT_FALSE(l.On(ViewerEvent::Tick, 5000).trimNow);
    l.On(ViewerEvent::PrewarmDone, 6000);
    EXPECT_FALSE(l.On(ViewerEvent::Tick, 6999).trimNow);
    EXPECT_TRUE(l.On(ViewerEvent::Tick, 7000).trimNow);
}

TEST(Lifecycle, IdleExitAfterConfiguredTime)
{
    Lifecycle l(30 * 60000, 0);
    l.On(ViewerEvent::PrewarmDone, 0);
    EXPECT_FALSE(l.On(ViewerEvent::Tick, 30 * 60000 - 1).exitNow);
    EXPECT_TRUE(l.On(ViewerEvent::Tick, 30 * 60000).exitNow);
}

TEST(Lifecycle, NoIdleExitBeforePrewarmDone)
{
    Lifecycle l(1000, 0);
    EXPECT_FALSE(l.On(ViewerEvent::Tick, 60000).exitNow);
}

TEST(Lifecycle, NeverWhenZero)
{
    Lifecycle l(0, 0);
    l.On(ViewerEvent::PrewarmDone, 0);
    EXPECT_FALSE(l.On(ViewerEvent::Tick, 1'000'000'000).exitNow);
}

TEST(Lifecycle, ShowResetsIdle)
{
    Lifecycle l(1000, 0);
    l.On(ViewerEvent::PrewarmDone, 0);
    l.On(ViewerEvent::PreviewShown, 500);
    EXPECT_FALSE(l.On(ViewerEvent::Tick, 800).exitNow);
    l.On(ViewerEvent::PreviewHidden, 900);
    EXPECT_FALSE(l.On(ViewerEvent::Tick, 1500).exitNow);
    EXPECT_FALSE(l.On(ViewerEvent::Tick, 1899).exitNow);
    EXPECT_TRUE(l.On(ViewerEvent::Tick, 1900).exitNow);
}

TEST(Lifecycle, PinnedBlocksExitAndTrim)
{
    Lifecycle l(1000, 0);
    l.On(ViewerEvent::PrewarmDone, 0);
    l.On(ViewerEvent::PinnedOpened, 0);
    auto a = l.On(ViewerEvent::Tick, 5000);
    EXPECT_FALSE(a.trimNow);
    EXPECT_FALSE(a.exitNow);
    EXPECT_FALSE(l.On(ViewerEvent::PinnedClosed, 5000).exitNow);
    auto b = l.On(ViewerEvent::Tick, 5999);
    EXPECT_FALSE(b.trimNow);
    EXPECT_FALSE(b.exitNow);
    auto c = l.On(ViewerEvent::Tick, 6000);
    EXPECT_TRUE(c.trimNow);
    EXPECT_TRUE(c.exitNow);
}

TEST(Lifecycle, TwoPinnedNeedBothClosed)
{
    Lifecycle l(1000, 0);
    l.On(ViewerEvent::PrewarmDone, 0);
    l.On(ViewerEvent::PinnedOpened, 10);
    l.On(ViewerEvent::PinnedOpened, 20);
    l.On(ViewerEvent::PinnedClosed, 30);
    EXPECT_FALSE(l.On(ViewerEvent::Tick, 9000).exitNow);
    l.On(ViewerEvent::PinnedClosed, 9000);
    EXPECT_TRUE(l.On(ViewerEvent::Tick, 10000).exitNow);
}

TEST(Lifecycle, SettingsWindowBlocksExit)
{
    Lifecycle l(1000, 0);
    l.On(ViewerEvent::PrewarmDone, 0);
    l.On(ViewerEvent::SettingsOpened, 0);
    auto a = l.On(ViewerEvent::Tick, 5000);
    EXPECT_FALSE(a.trimNow);
    EXPECT_FALSE(a.exitNow);
    l.On(ViewerEvent::SettingsClosed, 5000);
    EXPECT_FALSE(l.On(ViewerEvent::Tick, 5999).exitNow);
    auto c = l.On(ViewerEvent::Tick, 6000);
    EXPECT_TRUE(c.trimNow);
    EXPECT_TRUE(c.exitNow);
}

TEST(Lifecycle, ExitWhenQuiet)
{
    Lifecycle l(0, 0);
    l.On(ViewerEvent::PreviewShown, 0);
    l.SetExitWhenQuiet();
    EXPECT_FALSE(l.On(ViewerEvent::Tick, 100).exitNow);
    // Never inside the close itself: the closed previewer may still be releasing (ordered media close).
    EXPECT_FALSE(l.On(ViewerEvent::PreviewHidden, 200).exitNow);
    EXPECT_FALSE(l.On(ViewerEvent::Tick, 200 + kQuietExitDelayMs - 1).exitNow);
    EXPECT_TRUE(l.On(ViewerEvent::Tick, 200 + kQuietExitDelayMs).exitNow);
}

TEST(Lifecycle, ExitWhenQuietWaitsForAQuietTickAfterAReopen)
{
    Lifecycle l(0, 0);
    l.On(ViewerEvent::PrewarmDone, 0);
    l.SetExitWhenQuiet();
    l.On(ViewerEvent::PreviewShown, 100);
    EXPECT_FALSE(l.On(ViewerEvent::PreviewHidden, 200).exitNow);
    l.On(ViewerEvent::PreviewShown, 500); // reopened within the delay
    EXPECT_FALSE(l.On(ViewerEvent::Tick, 200 + kQuietExitDelayMs).exitNow);
    EXPECT_FALSE(l.On(ViewerEvent::PreviewHidden, 2000).exitNow);
    EXPECT_FALSE(l.On(ViewerEvent::Tick, 2000 + kQuietExitDelayMs - 1).exitNow);
    EXPECT_TRUE(l.On(ViewerEvent::Tick, 2000 + kQuietExitDelayMs).exitNow);
}

TEST(Lifecycle, ExitWhenQuietWaitsForPinnedWindows)
{
    Lifecycle l(0, 0);
    l.On(ViewerEvent::PrewarmDone, 0);
    l.On(ViewerEvent::PinnedOpened, 10);
    l.SetExitWhenQuiet();
    EXPECT_FALSE(l.On(ViewerEvent::Tick, 5000).exitNow);
    EXPECT_FALSE(l.On(ViewerEvent::PinnedClosed, 6000).exitNow);
    EXPECT_TRUE(l.On(ViewerEvent::Tick, 6000 + kQuietExitDelayMs).exitNow);
}

TEST(Lifecycle, ExitWhenQuietWhileAlreadyQuietExitsOnTheNextTick)
{
    Lifecycle l(0, 0);
    l.On(ViewerEvent::PrewarmDone, 0);
    l.SetExitWhenQuiet();
    EXPECT_TRUE(l.On(ViewerEvent::Tick, 5000).exitNow);
}

TEST(Lifecycle, PinnedJumpsBlockUntilEveryWindowClosed)
{
    Lifecycle l(1000, 0);
    l.On(ViewerEvent::PrewarmDone, 0);
    // 0 -> 2 in one step, then 2 -> 0 in one step (the host replays a count change as that many events).
    l.On(ViewerEvent::PinnedOpened, 10);
    l.On(ViewerEvent::PinnedOpened, 10);
    auto a = l.On(ViewerEvent::Tick, 9000);
    EXPECT_FALSE(a.trimNow);
    EXPECT_FALSE(a.exitNow);
    EXPECT_FALSE(l.On(ViewerEvent::PinnedClosed, 9000).exitNow);
    EXPECT_FALSE(l.On(ViewerEvent::Tick, 20000).exitNow);
    l.On(ViewerEvent::PinnedClosed, 20000);
    EXPECT_FALSE(l.On(ViewerEvent::Tick, 20999).exitNow);
    EXPECT_TRUE(l.On(ViewerEvent::Tick, 21000).exitNow);
}
