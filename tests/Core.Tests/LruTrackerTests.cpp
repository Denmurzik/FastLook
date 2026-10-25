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
