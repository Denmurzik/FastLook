#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "Ipc/Protocol.h"

namespace fastlook::core::nav
{
    inline constexpr size_t kMaxFolderItems = 20000, kWindowRadius = 5000;

    struct IndexRange
    {
        size_t begin = 0, end = 0;
        bool operator==(IndexRange const&) const = default;
    };

    // total <= kMaxFolderItems: everything; otherwise +-kWindowRadius around focus.
    IndexRange FolderWindow(size_t total, size_t focus);

    struct NavStart
    {
        std::vector<ipc::Item> items;
        uint32_t index = 0;
        ipc::NavMode mode = ipc::NavMode::Selection;
        bool listComplete = true;
    };

    std::optional<NavStart> PlanFromSelection(std::vector<ipc::Item> const& selectionInViewOrder, std::optional<ipc::Item> const& focused);

    // Case-insensitive path compare.
    std::optional<uint32_t> LocateItem(std::vector<ipc::Item> const& items, ipc::Item const& target);
}
