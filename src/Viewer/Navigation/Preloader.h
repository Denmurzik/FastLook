#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "Ipc/Protocol.h"
#include "Previewers/IPreviewer.h"

namespace fastlook::viewer
{
    // Neighbour cache (decision: +-1 item, at most kCapacity entries). Only Image and Text kinds are prepared, never cloud
    // placeholders; the previewer is prepared with PreviewContext::prefetch set: animated images and SVG are not prefetched, a text
    // file indexes only its first chunk. UI thread only.
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

        struct Taken
        {
            std::shared_ptr<IPreviewer> previewer;
            PreviewItem item;
            // Null: PrepareAsync has completed successfully. Otherwise it is still running and this action completes with it
            // (cancelling it cancels the preparation); the caller owns the previewer from now on and must Unload it afterwards.
            winrt::Windows::Foundation::IAsyncAction pending{nullptr};
        };

        // Removes the entry of `item` from the cache and hands it over: prepared, or - when its PrepareAsync is still running - to
        // be adopted by the caller. Nullopt when there is no usable entry (not cached, unsuitable, failed, still classifying).
        // The caller's context must match the one given to Prefetch (same monitor): the previewer keeps that context.
        std::optional<Taken> Take(core::ipc::Item const& item);

        // Drops everything (previewers whose PrepareAsync is still running are unloaded when it completes).
        void Clear();

        size_t Size() const { return m_entries.size(); }

    private:
        struct Entry;
        static void Retire(std::shared_ptr<Entry> const& entry);
        static winrt::fire_and_forget PrepareEntry(std::shared_ptr<Entry> entry, std::shared_ptr<PreviewContext const> ctx);
        static winrt::Windows::Foundation::IAsyncAction Await(winrt::Windows::Foundation::IAsyncAction inner);
        static void OnCompleted(std::shared_ptr<Entry> const& entry);

        std::vector<std::shared_ptr<Entry>> m_entries;
    };
}
