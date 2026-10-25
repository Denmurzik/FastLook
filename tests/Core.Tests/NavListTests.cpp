#include "Nav/NavList.h"
#include "TestUtil.h"

using namespace fastlook::core;
using namespace fastlook::core::nav;

namespace
{
    std::vector<ipc::Item> Many(size_t n)
    {
        std::vector<ipc::Item> v;
        v.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            v.push_back(ipc::Item{L"C:\\d\\" + std::to_wstring(i)});
        }
        return v;
    }
}

TEST(NavList, FolderWindowBounds)
{
    EXPECT_EQ(FolderWindow(0, 0), (IndexRange{0, 0}));
    EXPECT_EQ(FolderWindow(100, 50), (IndexRange{0, 100}));
    EXPECT_EQ(FolderWindow(20000, 19999), (IndexRange{0, 20000}));
    EXPECT_EQ(FolderWindow(20001, 0), (IndexRange{0, 5001}));
    EXPECT_EQ(FolderWindow(20001, 20000), (IndexRange{15000, 20001}));
    EXPECT_EQ(FolderWindow(100000, 50000), (IndexRange{45000, 55001}));
}

TEST(NavList, FolderWindowEdgesOfRadius)
{
    EXPECT_EQ(FolderWindow(100000, 5000), (IndexRange{0, 10001}));
    EXPECT_EQ(FolderWindow(100000, 5001), (IndexRange{1, 10002}));
    EXPECT_EQ(FolderWindow(100000, 99999), (IndexRange{94999, 100000}));
}

TEST(NavList, PlanFromSelection)
{
    std::vector<ipc::Item> sel{{L"C:\\a"}, {L"C:\\b"}, {L"C:\\c"}};
    auto p = PlanFromSelection(sel, ipc::Item{L"c:\\B"});
    ASSERT_TRUE(p);
    EXPECT_EQ(p->mode, ipc::NavMode::Selection);
    EXPECT_EQ(p->index, 1u);
    EXPECT_TRUE(p->listComplete);
    EXPECT_EQ(p->items, sel);
    EXPECT_EQ(PlanFromSelection(sel, ipc::Item{L"C:\\z"})->index, 0u);
    EXPECT_EQ(PlanFromSelection(sel, std::nullopt)->index, 0u);
    auto one = PlanFromSelection({sel[0]}, std::nullopt);
    EXPECT_EQ(one->mode, ipc::NavMode::Folder);
    EXPECT_FALSE(one->listComplete);
    EXPECT_EQ(PlanFromSelection({}, sel[2])->items.front(), sel[2]);
    EXPECT_FALSE(PlanFromSelection({}, std::nullopt));
}

TEST(NavList, SingleSelectionIgnoresFocusedAndIsFolderMode)
{
    std::vector<ipc::Item> sel{{L"C:\\a"}};
    auto p = PlanFromSelection(sel, ipc::Item{L"C:\\other"});
    ASSERT_TRUE(p);
    EXPECT_EQ(p->mode, ipc::NavMode::Folder);
    EXPECT_FALSE(p->listComplete);
    ASSERT_EQ(p->items.size(), 1u);
    EXPECT_EQ(p->items[0], sel[0]);
    EXPECT_EQ(p->index, 0u);
}

TEST(NavList, FocusedOnlyIsFolderMode)
{
    auto p = PlanFromSelection({}, ipc::Item{L"C:\\x"});
    ASSERT_TRUE(p);
    EXPECT_EQ(p->mode, ipc::NavMode::Folder);
    EXPECT_FALSE(p->listComplete);
    EXPECT_EQ(p->items.size(), 1u);
    EXPECT_EQ(p->index, 0u);
}

TEST(NavList, VirtualFlagPreserved)
{
    std::vector<ipc::Item> sel{{L"::{GUID}", true}, {L"C:\\b"}};
    auto p = PlanFromSelection(sel, sel[1]);
    ASSERT_TRUE(p);
    EXPECT_TRUE(p->items[0].isVirtual);
    EXPECT_EQ(p->index, 1u);
}

TEST(NavList, HugeSelectionIsWindowed)
{
    auto sel = Many(30000);
    auto p = PlanFromSelection(sel, sel[29000]);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->mode, ipc::NavMode::Selection);
    EXPECT_EQ(p->items.size(), 6000u); // window [24000, 30000) around focus 29000
}

TEST(NavList, HugeSelectionWindowContainsFocused)
{
    auto sel = Many(100000);
    auto p = PlanFromSelection(sel, sel[50000]);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->items.size(), 10001u);
    ASSERT_LT(p->index, p->items.size());
    EXPECT_EQ(p->items[p->index], sel[50000]);
    EXPECT_EQ(p->index, 5000u);
    EXPECT_TRUE(p->listComplete);
}

TEST(NavList, HugeSelectionNearEndKeepsFocusedIndexValid)
{
    auto sel = Many(30000);
    auto p = PlanFromSelection(sel, sel[29000]);
    ASSERT_TRUE(p);
    ASSERT_LT(p->index, p->items.size());
    EXPECT_EQ(p->items[p->index], sel[29000]);
    EXPECT_EQ(p->items.back(), sel.back());
}

TEST(NavList, SelectionAtCapIsNotWindowed)
{
    auto sel = Many(20000);
    auto p = PlanFromSelection(sel, sel[19999]);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->items.size(), 20000u);
    EXPECT_EQ(p->index, 19999u);
}

TEST(NavList, LocateItemIsCaseInsensitive)
{
    std::vector<ipc::Item> v{{L"C:\\Dir\\Файл.TXT"}, {L"C:\\dir\\b.txt"}};
    EXPECT_EQ(LocateItem(v, ipc::Item{L"c:\\DIR\\файл.txt"}), std::optional<uint32_t>(0));
    EXPECT_EQ(LocateItem(v, ipc::Item{L"C:\\DIR\\B.TXT"}), std::optional<uint32_t>(1));
    EXPECT_FALSE(LocateItem(v, ipc::Item{L"C:\\dir\\c.txt"}));
    EXPECT_FALSE(LocateItem({}, ipc::Item{L"C:\\a"}));
}
