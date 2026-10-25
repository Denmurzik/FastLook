#include "Nav/NavList.h"

#include <Windows.h>

#include <algorithm>

namespace fastlook::core::nav
{
    namespace
    {
        bool PathEquals(std::wstring const& a, std::wstring const& b)
        {
            if (a.size() != b.size()) return false;
            if (a.empty()) return true;
            return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
        }
    }

    IndexRange FolderWindow(size_t total, size_t focus)
    {
        if (total <= kMaxFolderItems) return {0, total};
        if (focus >= total) focus = total - 1;
        size_t begin = focus > kWindowRadius ? focus - kWindowRadius : 0;
        size_t end = std::min(total, focus + kWindowRadius + 1);
        return {begin, end};
    }

    std::optional<uint32_t> LocateItem(std::vector<ipc::Item> const& items, ipc::Item const& target)
    {
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (PathEquals(items[i].path, target.path)) return static_cast<uint32_t>(i);
        }
        return std::nullopt;
    }

    std::optional<NavStart> PlanFromSelection(std::vector<ipc::Item> const& sel, std::optional<ipc::Item> const& focused)
    {
        NavStart out;
        if (sel.size() > 1)
        {
            size_t pos = focused ? LocateItem(sel, *focused).value_or(0) : 0;
            auto w = FolderWindow(sel.size(), pos);
            out.items.assign(sel.begin() + static_cast<std::ptrdiff_t>(w.begin), sel.begin() + static_cast<std::ptrdiff_t>(w.end));
            out.index = static_cast<uint32_t>(pos - w.begin);
            out.mode = ipc::NavMode::Selection;
            out.listComplete = true;
            return out;
        }
        out.mode = ipc::NavMode::Folder;
        out.listComplete = false;
        if (sel.size() == 1)
        {
            out.items = {sel[0]};
            return out;
        }
        if (focused)
        {
            out.items = {*focused};
            return out;
        }
        return std::nullopt;
    }
}
