#include "pch.h"
#include "Navigation/Preloader.h"

#include <algorithm>

#include "Diagnostics/Log.h"
#include "FileTypes/ClassifyFile.h"
#include "Previewers/PreviewerRegistry.h"
#include "Util/Async.h"

namespace fastlook::viewer
{
    namespace
    {
        using core::PreviewerKind;

        bool SamePath(std::wstring_view a, std::wstring_view b)
        {
            return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
        }

        struct ComScope
        {
            bool initialised;
            ComScope() : initialised(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
            ~ComScope()
            {
                if (initialised)
                {
                    CoUninitialize();
                }
            }
        };

        // Background thread. Anything but a readable, non-placeholder Image/Text file is "unsuitable" (the session classifies it
        // itself when it is navigated to).
        std::optional<core::FileClassification> ClassifyForPrefetch(core::ipc::Item const& ref, bool useShellHandlers)
        {
            try
            {
                if (!ref.isVirtual && GetFileAttributesW(ref.path.c_str()) == INVALID_FILE_ATTRIBUTES)
                {
                    return std::nullopt;
                }
                ComScope com;
                auto classification = core::ClassifyFile(ref.path, ref.isVirtual, useShellHandlers);
                if (classification.cloudPlaceholder || (classification.kind != PreviewerKind::Image && classification.kind != PreviewerKind::Text))
                {
                    return std::nullopt;
                }
                return classification;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }
    }

    struct Preloader::Entry
    {
        core::ipc::Item ref;
        PreviewItem item;
        std::shared_ptr<IPreviewer> previewer;
        winrt::Windows::Foundation::IAsyncAction action{nullptr};
        bool completed = false; // PrepareAsync is over (or there was nothing to prepare)
        bool usable = false;    // completed successfully and not retired
        bool retired = false;
        bool adopted = false;   // Take() handed the running PrepareAsync to the session: this entry only observes its end
    };

    Preloader::Preloader() = default;

    Preloader::~Preloader()
    {
        Clear();
    }

    void Preloader::Prefetch(std::vector<core::ipc::Item> neighbours, std::shared_ptr<PreviewContext const> ctx)
    {
        if (neighbours.size() > kCapacity)
        {
            neighbours.resize(kCapacity);
        }
        auto const listed = [&](Entry const& entry) {
            return std::ranges::any_of(neighbours, [&](core::ipc::Item const& item) { return SamePath(item.path, entry.ref.path); });
        };
        for (auto it = m_entries.begin(); it != m_entries.end();)
        {
            if (listed(**it))
            {
                ++it;
            }
            else
            {
                Retire(*it);
                it = m_entries.erase(it);
            }
        }
        if (!ctx)
        {
            return;
        }

        auto prefetchCtx = std::make_shared<PreviewContext>(*ctx);
        prefetchCtx->prefetch = true;
        for (auto const& item : neighbours)
        {
            bool const present = std::ranges::any_of(m_entries, [&](auto const& entry) { return SamePath(entry->ref.path, item.path); });
            if (present)
            {
                continue;
            }
            auto entry = std::make_shared<Entry>();
            entry->ref = item;
            m_entries.push_back(entry);
            PrepareEntry(entry, prefetchCtx);
        }
    }

    std::optional<Preloader::Taken> Preloader::Take(core::ipc::Item const& item)
    {
        auto const it = std::ranges::find_if(m_entries, [&](auto const& entry) { return SamePath(entry->ref.path, item.path); });
        if (it == m_entries.end())
        {
            return std::nullopt;
        }
        auto entry = std::move(*it);
        m_entries.erase(it);
        if (entry->usable)
        {
            return Taken{std::move(entry->previewer), entry->item, nullptr};
        }
        if (!entry->completed && !entry->retired && entry->previewer && entry->action)
        {
            entry->adopted = true;
            return Taken{entry->previewer, entry->item, Await(entry->action)};
        }
        Retire(entry); // failed, unsuitable or still classifying
        return std::nullopt;
    }

    winrt::Windows::Foundation::IAsyncAction Preloader::Await(winrt::Windows::Foundation::IAsyncAction inner)
    {
        // C++/WinRT does not forward cancellation to an awaited action unless asked: without this, cancelling the wrapper (Retire)
        // would leave the adopted PrepareAsync running.
        auto const token = co_await winrt::get_cancellation_token();
        token.enable_propagation();
        co_await inner; // a failure of `inner` rethrows here
    }

    void Preloader::Clear()
    {
        for (auto& entry : m_entries)
        {
            Retire(entry);
        }
        m_entries.clear();
    }

    // The previewer is unloaded here when its PrepareAsync is over, otherwise in OnCompleted (decision 54).
    void Preloader::Retire(std::shared_ptr<Entry> const& entry)
    {
        if (entry->retired)
        {
            return;
        }
        entry->retired = true;
        entry->usable = false;
        if (entry->action)
        {
            try
            {
                entry->action.Cancel();
            }
            catch (...)
            {
            }
        }
        if (entry->completed)
        {
            if (auto previewer = std::move(entry->previewer))
            {
                try
                {
                    previewer->Unload();
                }
                catch (...)
                {
                }
            }
        }
    }

    void Preloader::OnCompleted(std::shared_ptr<Entry> const& entry)
    {
        entry->completed = true;
        if (entry->adopted)
        {
            entry->action = nullptr; // the session owns the previewer and unloads it
            return;
        }
        bool failed = false;
        try
        {
            entry->action.GetResults();
        }
        catch (...)
        {
            failed = true; // includes a cancelled run
        }
        entry->action = nullptr; // breaks the entry -> action -> handler -> entry cycle
        if (!entry->retired && !failed)
        {
            entry->usable = true;
            return;
        }
        if (auto previewer = std::move(entry->previewer))
        {
            try
            {
                previewer->Unload();
            }
            catch (...)
            {
            }
        }
    }

    winrt::fire_and_forget Preloader::PrepareEntry(std::shared_ptr<Entry> entry, std::shared_ptr<PreviewContext const> ctx)
    {
        auto const ui = ctx->ui;
        bool const useShellHandlers = ctx->settings.preview.useShellHandlers;
        // A fire_and_forget coroutine terminates the process on an escaping exception: nothing may leave this try block.
        try
        {
            co_await winrt::resume_background();
            auto const classification = ClassifyForPrefetch(entry->ref, useShellHandlers);
            if (!co_await ResumeOn(ui))
            {
                co_return;
            }
            if (entry->retired || !classification)
            {
                entry->completed = true; // unsuitable: kept (unusable) so the neighbour is not classified again
                co_return;
            }

            entry->item.ref = entry->ref;
            entry->item.kind = classification->kind;
            entry->item.shellHandlerClsid = classification->shellHandlerClsid;
            entry->item.cloudPlaceholder = classification->cloudPlaceholder;
            entry->previewer = CreatePreviewer(classification->kind);
            entry->action = entry->previewer->PrepareAsync(entry->item, std::move(ctx));
            entry->action.Completed([entry, ui](auto const&, auto) {
                ui.TryEnqueue([entry] { OnCompleted(entry); });
            });
        }
        catch (...)
        {
            FL_LOG_INFO(L"Preloading a neighbour failed");
            entry->completed = true;
            entry->action = nullptr;
            if (auto previewer = std::move(entry->previewer))
            {
                try
                {
                    previewer->Unload();
                }
                catch (...)
                {
                }
            }
        }
    }
}
