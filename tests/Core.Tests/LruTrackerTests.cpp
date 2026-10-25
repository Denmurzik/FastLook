#include <gtest/gtest.h>

#include "Util/LruTracker.h"

TEST(LruTracker, EvictsLeastRecent)
{
    fastlook::core::LruTracker<int> l(3);
    l.Touch(1);
    l.Touch(2);
    l.Touch(3);
    l.Touch(1);
    EXPECT_EQ(l.Touch(4), 2);
    EXPECT_TRUE(l.Contains(1));
    EXPECT_FALSE(l.Contains(2));
    EXPECT_EQ(l.Size(), 3u);
}

TEST(LruTracker, TouchExistingDoesNotEvict)
{
    fastlook::core::LruTracker<int> l(2);
    EXPECT_EQ(l.Touch(1), std::nullopt);
    EXPECT_EQ(l.Touch(2), std::nullopt);
    EXPECT_EQ(l.Touch(2), std::nullopt);
    EXPECT_EQ(l.Size(), 2u);
}

TEST(LruTracker, ClearEmpties)
{
    fastlook::core::LruTracker<int> l(2);
    l.Touch(1);
    l.Clear();
    EXPECT_EQ(l.Size(), 0u);
    EXPECT_FALSE(l.Contains(1));
}

TEST(LruTracker, EvictionOrderFollowsTouches)
{
    fastlook::core::LruTracker<int> l(2);
    l.Touch(1);
    l.Touch(2);
    EXPECT_EQ(l.Touch(3), 1);
    EXPECT_EQ(l.Touch(4), 2);
    EXPECT_EQ(l.Touch(5), 3);
}

TEST(LruTracker, ProtectedKeysAreNeverEvicted)
{
    fastlook::core::LruTracker<int> l(2);
    l.Touch(1);
    l.Touch(2);
    auto const keep1 = [](int const& k) { return k == 1; };
    EXPECT_EQ(l.TouchProtected(3, keep1), 2); // 1 is the oldest but protected
    EXPECT_TRUE(l.Contains(1));
    EXPECT_TRUE(l.Contains(3));
    EXPECT_EQ(l.Size(), 2u);
}

TEST(LruTracker, AllProtectedGrowsPastCapacity)
{
    fastlook::core::LruTracker<int> l(2);
    l.Touch(1);
    l.Touch(2);
    EXPECT_EQ(l.TouchProtected(3, [](int const&) { return true; }), std::nullopt);
    EXPECT_EQ(l.Size(), 3u);
    EXPECT_TRUE(l.Contains(1));
}

TEST(LruTracker, OldestIfAndErase)
{
    fastlook::core::LruTracker<int> l(4);
    l.Touch(1);
    l.Touch(2);
    l.Touch(3);
    EXPECT_EQ(l.OldestIf([](int const& k) { return k > 1; }), 2);
    EXPECT_EQ(l.OldestIf([](int const& k) { return k > 5; }), std::nullopt);
    EXPECT_TRUE(l.Erase(2));
    EXPECT_FALSE(l.Erase(2));
    EXPECT_EQ(l.Size(), 2u);
}

TEST(LruTracker, ZeroCapacityHoldsNothing)
{
    fastlook::core::LruTracker<int> l(0);
    EXPECT_EQ(l.Touch(7), 7);
    EXPECT_EQ(l.Size(), 0u);
    EXPECT_FALSE(l.Contains(7));
}
