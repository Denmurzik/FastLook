#include "pch.h"
#include "Modes/Modes.h"

#include <psapi.h>
#include <winternl.h> // NTSTATUS for d3dkmthk.h
#include <d3dkmthk.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Diagnostics/Log.h"
#include "FileTypes/ClassifyFile.h"
#include "System/AppPaths.h"
#include "Text/Utf.h"
#include "Util/Async.h"
#include "Util/Trim.h"
#include "ViewerHost.h"

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "gdi32.lib") // D3DKMT

namespace fastlook::viewer
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr auto kContentCap = std::chrono::seconds(5);
        constexpr auto kPoll = std::chrono::milliseconds(2);
        constexpr auto kSettle = std::chrono::seconds(1);
        constexpr auto kCloseCap = std::chrono::seconds(2);
        constexpr uint32_t kSeriesEvery = 50;
        constexpr uint32_t kSampleAt = 100;

        double Ms(LARGE_INTEGER from, LARGE_INTEGER to)
        {
            static LARGE_INTEGER const freq = [] {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                return f;
            }();
            return static_cast<double>(to.QuadPart - from.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
        }

        LARGE_INTEGER Now()
        {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            return t;
        }

        double Percentile(std::vector<double> values, double p)
        {
            if (values.empty())
            {
                return 0;
            }
            std::sort(values.begin(), values.end());
            auto const index = static_cast<size_t>(std::ceil(p * static_cast<double>(values.size()))) - 1;
            return values[std::min(index, values.size() - 1)];
        }

        constexpr double kMB = 1024.0 * 1024.0;

        // Private Bytes split by owner. "Driver" is the graphics kernel's account of this process's GPU allocations in system
        // memory (D3DKMT process statistics, all adapters): on an integrated GPU the user-mode driver backs them with private
        // committed memory and keeps released ones in pools. "Own" is what FastLook allocates and can leak: heap busy bytes. The
        // private commit outside the heaps is reported separately: it cannot be split exactly between the driver and the rest
        // (D3DKMT counts more than the private commit it causes), and in the measurements it grew only together with the driver.
        struct Memory
        {
            double privateWsMB = 0;
            double privateBytesMB = 0;
            double driverMB = 0;
            double heapBusyMB = 0;
            double nonHeapPrivateMB = 0;
            uint32_t handles = 0;

            double OwnMB() const { return heapBusyMB; }
        };

        double DriverSystemMemoryMB()
        {
            D3DKMT_ENUMADAPTERS2 enumerate{};
            if (D3DKMTEnumAdapters2(&enumerate) != 0 || enumerate.NumAdapters == 0)
            {
                return 0;
            }
            std::vector<D3DKMT_ADAPTERINFO> adapters(enumerate.NumAdapters);
            enumerate.pAdapters = adapters.data();
            if (D3DKMTEnumAdapters2(&enumerate) != 0)
            {
                return 0;
            }
            uint64_t bytes = 0;
            for (ULONG i = 0; i < enumerate.NumAdapters; ++i)
            {
                D3DKMT_QUERYSTATISTICS query{};
                query.Type = D3DKMT_QUERYSTATISTICS_PROCESS;
                query.AdapterLuid = adapters[i].AdapterLuid;
                query.hProcess = GetCurrentProcess();
                if (D3DKMTQueryStatistics(&query) == 0)
                {
                    bytes += query.QueryResult.ProcessInformation.SystemMemory.BytesAllocated;
                }
                D3DKMT_CLOSEADAPTER close{adapters[i].hAdapter};
                D3DKMTCloseAdapter(&close);
            }
            return static_cast<double>(bytes) / kMB;
        }

        // Busy bytes of every process heap, and the committed private memory that belongs to no heap (VirtualQuery over the
        // address space; an allocation that holds a heap region or a large heap block is heap memory).
        void HeapStats(Memory& memory)
        {
            std::vector<HANDLE> heaps(GetProcessHeaps(0, nullptr) + 16);
            DWORD const count = std::min<DWORD>(GetProcessHeaps(static_cast<DWORD>(heaps.size()), heaps.data()), static_cast<DWORD>(heaps.size()));
            uint64_t busy = 0;
            std::vector<std::pair<uintptr_t, uintptr_t>> ranges; // [begin, end) of heap regions and large blocks
            for (DWORD i = 0; i < count; ++i)
            {
                if (!HeapLock(heaps[i]))
                {
                    continue;
                }
                PROCESS_HEAP_ENTRY entry{};
                uintptr_t regionBegin = 0;
                uintptr_t regionEnd = 0;
                while (HeapWalk(heaps[i], &entry))
                {
                    auto const data = reinterpret_cast<uintptr_t>(entry.lpData);
                    if (entry.wFlags & PROCESS_HEAP_REGION)
                    {
                        regionBegin = data;
                        regionEnd = reinterpret_cast<uintptr_t>(entry.Region.lpLastBlock);
                        ranges.emplace_back(regionBegin, regionEnd);
                    }
                    else if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)
                    {
                        busy += entry.cbData;
                        if (data < regionBegin || data >= regionEnd)
                        {
                            ranges.emplace_back(data, data + entry.cbData);
                        }
                    }
                }
                HeapUnlock(heaps[i]);
            }
            std::sort(ranges.begin(), ranges.end());
            auto const inHeap = [&](uintptr_t begin, uintptr_t end) {
                auto it = std::upper_bound(ranges.begin(), ranges.end(), std::pair{end, uintptr_t{0}});
                while (it != ranges.begin())
                {
                    --it;
                    if (it->second > begin)
                    {
                        return true;
                    }
                    if (end - it->first > (uintptr_t{1} << 36))
                    {
                        break; // far below: no heap range can overlap any more
                    }
                }
                return false;
            };

            uint64_t nonHeap = 0;
            MEMORY_BASIC_INFORMATION mbi{};
            uintptr_t address = 0;
            uintptr_t allocationBase = 0;
            uintptr_t allocationEnd = 0;
            uint64_t allocationCommitted = 0;
            auto const flush = [&] {
                if (allocationCommitted && !inHeap(allocationBase, allocationEnd))
                {
                    nonHeap += allocationCommitted;
                }
                allocationCommitted = 0;
            };
            while (VirtualQuery(reinterpret_cast<void const*>(address), &mbi, sizeof(mbi)) == sizeof(mbi))
            {
                auto const base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
                if (mbi.State != MEM_FREE && mbi.Type == MEM_PRIVATE)
                {
                    if (base != allocationBase)
                    {
                        flush();
                        allocationBase = base;
                    }
                    allocationEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                    if (mbi.State == MEM_COMMIT)
                    {
                        allocationCommitted += mbi.RegionSize;
                    }
                }
                address = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            }
            flush();
            memory.heapBusyMB = static_cast<double>(busy) / kMB;
            memory.nonHeapPrivateMB = static_cast<double>(nonHeap) / kMB;
        }

        Memory SampleMemory()
        {
            PROCESS_MEMORY_COUNTERS_EX2 counters{};
            counters.cb = sizeof(counters);
            Memory memory;
            if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
            {
                memory.privateWsMB = static_cast<double>(counters.PrivateWorkingSetSize) / kMB;
                memory.privateBytesMB = static_cast<double>(counters.PrivateUsage) / kMB;
            }
            memory.driverMB = DriverSystemMemoryMB();
            HeapStats(memory);
            DWORD handles = 0;
            GetProcessHandleCount(GetCurrentProcess(), &handles);
            memory.handles = handles;
            return memory;
        }

        char const* KindName(core::PreviewerKind kind)
        {
            switch (kind)
            {
            case core::PreviewerKind::Image: return "image";
            case core::PreviewerKind::Text: return "text";
            case core::PreviewerKind::Markdown: return "markdown";
            case core::PreviewerKind::Media: return "media";
            case core::PreviewerKind::Pdf: return "pdf";
            case core::PreviewerKind::Archive: return "archive";
            case core::PreviewerKind::Folder: return "folder";
            case core::PreviewerKind::Shell: return "shell";
            case core::PreviewerKind::Fallback: return "fallback";
            }
            return "other";
        }

        struct Entry
        {
            std::wstring path;
            std::string kind;
            bool directory = false;
        };

        std::vector<Entry> ListEntries(std::wstring const& dir)
        {
            std::vector<Entry> entries;
            std::error_code ec;
            for (auto const& item : fs::directory_iterator(dir, ec))
            {
                Entry entry;
                entry.path = fs::absolute(item.path(), ec).wstring();
                entry.directory = item.is_directory(ec);
                entry.kind = entry.directory ? "folder" : KindName(core::ClassifyFile(entry.path, false, false).kind);
                entries.push_back(std::move(entry));
            }
            std::sort(entries.begin(), entries.end(), [](Entry const& a, Entry const& b) { return a.path < b.path; });
            return entries;
        }

        struct Results
        {
            uint32_t iterations = 0;
            uint32_t failures = 0;
            std::map<std::string, std::vector<double>> contentMs;
            std::map<std::string, std::vector<double>> showMs;
            Memory at100;
            Memory end;
            bool hasAt100 = false;
            bool hasEnd = false;
            std::vector<std::pair<uint32_t, Memory>> series; // every kSeriesEvery iterations
        };

        std::string Num(double value)
        {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%.2f", value);
            return buffer;
        }

        void WriteJson(std::wstring const& path, Results const& r)
        {
            std::string json = "{\"iterations\":" + std::to_string(r.iterations) + ",\"failures\":" + std::to_string(r.failures) + ",\"perKind\":{";
            bool first = true;
            for (auto const& [kind, content] : r.contentMs)
            {
                if (!first)
                {
                    json += ',';
                }
                first = false;
                auto const show = r.showMs.find(kind);
                json += "\"" + kind + "\":{\"count\":" + std::to_string(content.size()) +
                        ",\"p50ContentMs\":" + Num(Percentile(content, 0.50)) + ",\"p95ContentMs\":" + Num(Percentile(content, 0.95)) +
                        ",\"p50ShowMs\":" + Num(show == r.showMs.end() ? 0 : Percentile(show->second, 0.50)) + "}";
            }
            // null = the sample was not taken (fewer than 100 iterations, or the run aborted): never a number that reads as zero growth.
            auto const opt = [](bool has, double value) { return has ? Num(value) : std::string{"null"}; };
            json += "},\"privateBytesAt100MB\":" + opt(r.hasAt100, r.at100.privateBytesMB) +
                    ",\"privateBytesEndMB\":" + opt(r.hasEnd, r.end.privateBytesMB) + ",\"privateWsAt100MB\":" + opt(r.hasAt100, r.at100.privateWsMB) +
                    ",\"privateWsEndMB\":" + opt(r.hasEnd, r.end.privateWsMB);
            json += ",\"ownAt100MB\":" + opt(r.hasAt100, r.at100.OwnMB()) + ",\"ownEndMB\":" + opt(r.hasEnd, r.end.OwnMB()) +
                    ",\"driverAt100MB\":" + opt(r.hasAt100, r.at100.driverMB) + ",\"driverEndMB\":" + opt(r.hasEnd, r.end.driverMB) +
                    ",\"nonHeapAt100MB\":" + opt(r.hasAt100, r.at100.nonHeapPrivateMB) + ",\"nonHeapEndMB\":" + opt(r.hasEnd, r.end.nonHeapPrivateMB) +
                    ",\"handlesAt100\":" + (r.hasAt100 ? std::to_string(r.at100.handles) : std::string{"null"}) +
                    ",\"handlesEnd\":" + (r.hasEnd ? std::to_string(r.end.handles) : std::string{"null"}) + ",\"series\":[";
            for (size_t i = 0; i < r.series.size(); ++i)
            {
                auto const& [iteration, m] = r.series[i];
                json += (i ? "," : "") + std::string{"{\"i\":"} + std::to_string(iteration) + ",\"privateBytesMB\":" + Num(m.privateBytesMB) +
                        ",\"ownMB\":" + Num(m.OwnMB()) + ",\"driverMB\":" + Num(m.driverMB) + ",\"nonHeapMB\":" + Num(m.nonHeapPrivateMB) + ",\"heapBusyMB\":" + Num(m.heapBusyMB) + ",\"handles\":" + std::to_string(m.handles) + "}";
            }
            json += "]}\n";
            std::ofstream file(fs::path(path), std::ios::binary | std::ios::trunc);
            file << json;
        }

        winrt::fire_and_forget RunBench(core::ViewerArgs args)
        {
            auto& host = ViewerHost::Get();
            auto const ui = host.Ui();
            Results results;
            try
            {
                auto const entries = ListEntries(args.benchDir);
                if (entries.empty())
                {
                    FL_LOG_INFO(L"Bench: no files in the bench directory");
                }
                else
                {
                    // Hidden prewarm; the host reports completion through its callback, a fixed pause is enough for the bench.
                    host.PrewarmHidden();
                    co_await winrt::resume_after(std::chrono::seconds(2));
                    if (!co_await ResumeOn(ui))
                    {
                        co_return;
                    }

                    struct Ready
                    {
                        LARGE_INTEGER at{};
                        bool ready = false;
                        bool hidden = false;
                    };
                    auto const state = std::make_shared<Ready>();
                    host.contentReady = [state] {
                        state->at = Now();
                        state->ready = true;
                    };
                    HostCallbacks callbacks;
                    callbacks.previewHidden = [state] { state->hidden = true; };
                    host.SetCallbacks(std::move(callbacks));

                    for (uint32_t i = 0; i < args.iterations; ++i)
                    {
                        auto const& entry = entries[i % entries.size()];
                        core::ipc::ShowPayload payload;
                        payload.items.push_back({entry.path, false});
                        payload.navMode = core::ipc::NavMode::Selection;

                        state->ready = false;
                        state->hidden = false;
                        auto const start = Now();
                        host.Show(payload);
                        auto const shown = Now();

                        // The cap is a wall-clock deadline: resume_after wakes at timer granularity (~16 ms), so counting polls would stretch it.
                        auto const deadline = std::chrono::steady_clock::now() + kContentCap;
                        while (!state->ready && std::chrono::steady_clock::now() < deadline)
                        {
                            co_await winrt::resume_after(kPoll);
                            if (!co_await ResumeOn(ui))
                            {
                                host.contentReady = nullptr;
                                co_return;
                            }
                        }
                        bool const ready = state->ready;
                        auto const readyAt = state->at;
                        if (ready)
                        {
                            results.showMs[entry.kind].push_back(Ms(start, shown));
                            results.contentMs[entry.kind].push_back(Ms(start, readyAt));
                        }
                        else
                        {
                            ++results.failures;
                            FL_LOG_INFO(L"Bench: content did not become ready within 5 s");
                        }
                        ++results.iterations;

                        // Open/close: the next open starts only once this preview has really closed (close animation, window hidden,
                        // content unloaded). Showing again at once would cancel the close and measure replacing content instead.
                        host.Hide();
                        auto const closeDeadline = std::chrono::steady_clock::now() + kCloseCap;
                        while (!state->hidden && std::chrono::steady_clock::now() < closeDeadline)
                        {
                            co_await winrt::resume_after(kPoll);
                            if (!co_await ResumeOn(ui))
                            {
                                host.contentReady = nullptr;
                                co_return;
                            }
                        }
                        if (!state->hidden)
                        {
                            ++results.failures;
                            FL_LOG_INFO(L"Bench: the preview did not close within 2 s");
                        }
                        TrimWorkingSet();
                        if (results.iterations % kSeriesEvery == 0 && results.iterations != kSampleAt)
                        {
                            // Settled like the sample at 100, so the series points compare with it (a closed preview finishes
                            // releasing its media and surfaces shortly after the window is hidden).
                            co_await winrt::resume_after(kSettle);
                            if (!co_await ResumeOn(ui))
                            {
                                co_return;
                            }
                            results.series.emplace_back(results.iterations, SampleMemory());
                        }
                        if (results.iterations == kSampleAt)
                        {
                            // Warm-idle sample, taken like the one at the end: the closed preview has finished releasing (1 s),
                            // the idle trim runs, and the working set settles for 1 s before Private WS is read.
                            co_await winrt::resume_after(kSettle);
                            if (!co_await ResumeOn(ui))
                            {
                                co_return;
                            }
                            TrimWorkingSet();
                            co_await winrt::resume_after(kSettle);
                            if (!co_await ResumeOn(ui))
                            {
                                co_return;
                            }
                            results.at100 = SampleMemory();
                            results.hasAt100 = true;
                            results.series.emplace_back(results.iterations, results.at100);
                        }
                    }

                    host.contentReady = nullptr;
                    host.Hide();
                    // Warm-idle sample (see the one at 100): settled since the last close, idle trim, 1 s.
                    co_await winrt::resume_after(kSettle);
                    if (!co_await ResumeOn(ui))
                    {
                        co_return;
                    }
                    TrimWorkingSet();
                    co_await winrt::resume_after(kSettle);
                    if (!co_await ResumeOn(ui))
                    {
                        co_return;
                    }
                    results.hasEnd = true;
                    results.end = SampleMemory();
                }
            }
            catch (winrt::hresult_error const& e)
            {
                ++results.failures;
                FL_LOG_INFO(L"Bench aborted at iteration {}: 0x{:08X} {}", results.iterations, static_cast<uint32_t>(e.code().value), e.message().c_str());
            }
            catch (std::exception const& e)
            {
                ++results.failures;
                FL_LOG_INFO(L"Bench aborted at iteration {}: {}", results.iterations, core::FromUtf8(e.what()));
            }
            catch (...)
            {
                ++results.failures;
                FL_LOG_INFO(L"Bench aborted at iteration {} by an exception", results.iterations);
            }

            if (!args.benchOut.empty())
            {
                WriteJson(args.benchOut, results);
            }
            host.ExitApp();
        }
    }

    void RunBenchMode(core::ViewerArgs const& args)
    {
        auto& host = ViewerHost::Get();
        host.Initialize(core::CurrentAppPaths(), CloseBehavior::HideWarm, args); // Hide() must keep the window for the next open
        host.Window();
        RunBench(args);
    }
}
