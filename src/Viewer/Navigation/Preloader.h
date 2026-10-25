#pragma once

#include <memory>
#include <vector>

#include "Ipc/Protocol.h"
#include "Previewers/IPreviewer.h"

namespace fastlook::viewer
{
    // Neighbour cache (decision: +-1 item, at most kCapacity entries). Only Image and Text kinds are prepared, never cloud
    // placeholders; the previewer is prepared with PreviewContext::prefetch set, so it does not animate or play and a text file
    // indexes only its first chunk. UI thread only.
    //
    // A previewer dropped by Clear()/Prefetch()/Take() while its PrepareAsync is still running is cancelled and unloaded only once
    // that PrepareAsync has completed (decision 54).
    class Preloader
    {
    public:
        static constexpr size_t kCapacity = 3;

        Preloader();
        ~Preloader();
        Preloader(Preloader const&) = delete;
        Preloader& operator=(Preloader const&) = delete;

        // Makes `neighbours` (at most kCapacity, in priority order) the cache content: entries not listed are dropped, missing ones
        // are classified off the UI thread and prepared. `ctx` is copied; its prefetch flag is set.
        void Prefetch(std::vector<core::ipc::Item> neighbours, std::shared_ptr<PreviewContext const> ctx);

        // The prepared previewer of `item` (removed from the cache), or nullptr. An entry whose PrepareAsync is still running is
        // dropped and nullptr is returned. `itemOut` receives the classified item. The caller's context must match the one given to
        // Prefetch (same monitor): the previewer keeps that context.
        std::shared_ptr<IPreviewer> Take(core::ipc::Item const& item, PreviewItem* itemOut = nullptr);

        // Drops everything (previewers whose PrepareAsync is still running are unloaded when it completes).
        void Clear();

        size_t Size() const { return m_entries.size(); }

    private:
        struct Entry;
        static void Retire(std::shared_ptr<Entry> const& entry);
        static winrt::fire_and_forget PrepareEntry(std::shared_ptr<Entry> entry, std::shared_ptr<PreviewContext const> ctx);
        static void OnCompleted(std::shared_ptr<Entry> const& entry);

        std::vector<std::shared_ptr<Entry>> m_entries;
    };
}
