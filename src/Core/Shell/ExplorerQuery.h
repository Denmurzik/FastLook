#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "Geometry.h"
#include "Ipc/Protocol.h"

namespace fastlook::core
{
    struct ExplorerSnapshot
    {
        HWND explorer = nullptr;
        bool isDesktop = false;
        std::vector<ipc::Item> selection;
        std::optional<ipc::Item> focused;
        size_t totalItems = 0;
        std::optional<RectI> iconRect;
        HMONITOR monitor = nullptr;
    };

    struct FolderList
    {
        std::vector<ipc::Item> items;
        uint32_t index = 0;
    };

    // All functions below run on an STA thread (ShellWorker) and may block on Explorer.

    // Fast phase: selection + focus of the active tab of the foreground Explorer window (or the desktop).
    // foreground == nullptr only primes the cached IShellWindows and returns nullopt.
    std::optional<ExplorerSnapshot> QueryExplorer(HWND foreground, bool isDesktop);

    // Folder contents in view order; when the folder has more than nav::kMaxFolderItems items, only
    // +-nav::kWindowRadius around the anchor. nullopt when the view or the anchor cannot be found.
    std::optional<FolderList> EnumerateFolder(HWND explorer, bool isDesktop, ipc::Item const& anchor);

    // Selects (and focuses) the item in the Explorer view, deselecting everything else.
    bool SelectInExplorer(HWND explorer, bool isDesktop, ipc::Item const& item);
}
