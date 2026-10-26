#include "pch.h"
#include "Modes/Modes.h"

#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
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

namespace fastlook::viewer
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr auto kContentCap = std::chrono::seconds(5);
        constexpr auto kPoll = std::chrono::milliseconds(2);
        constexpr auto kSettle = std::chrono::seconds(1);
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

        struct Memory
        {
            double privateWsMB = 0;
            double privateBytesMB = 0;
        };

        Memory SampleMemory()
        {
            PROCESS_MEMORY_COUNTERS_EX2 counters{};
            counters.cb = sizeof(counters);
            Memory memory;
            if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
            {
                memory.privateWsMB = static_cast<double>(counters.PrivateWorkingSetSize) / (1024.0 * 1024.0);
                memory.privateBytesMB = static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0);
            }
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
            json += "},\"privateBytesAt100MB\":" + Num(r.hasAt100 ? r.at100.privateBytesMB : 0) +
                    ",\"privateBytesEndMB\":" + Num(r.end.privateBytesMB) + ",\"privateWsAt100MB\":" + Num(r.hasAt100 ? r.at100.privateWsMB : 0) +
                    ",\"privateWsEndMB\":" + Num(r.end.privateWsMB) + "}\n";
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

                    LARGE_INTEGER readyAt{};
                    bool ready = false;
                    host.contentReady = [&] {
                        readyAt = Now();
                        ready = true;
                    };

                    for (uint32_t i = 0; i < args.iterations; ++i)
                    {
                        auto const& entry = entries[i % entries.size()];
                        core::ipc::ShowPayload payload;
                        payload.items.push_back({entry.path, false});
                        payload.navMode = core::ipc::NavMode::Selection;

                        ready = false;
                        auto const start = Now();
                        host.Show(payload);
                        auto const shown = Now();

                        for (auto waited = std::chrono::milliseconds(0); !ready && waited < kContentCap; waited += kPoll)
                        {
                            co_await winrt::resume_after(kPoll);
                            if (!co_await ResumeOn(ui))
                            {
                                co_return;
                            }
                        }
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

                        host.Hide();
                        TrimWorkingSet();
                        if (results.iterations == kSampleAt)
                        {
                            co_await winrt::resume_after(kSettle);
                            if (!co_await ResumeOn(ui))
                            {
                                co_return;
                            }
                            results.at100 = SampleMemory();
                            results.hasAt100 = true;
                        }
                    }

                    host.contentReady = nullptr;
                    host.Hide();
                    TrimWorkingSet();
                    co_await winrt::resume_after(kSettle);
                    if (!co_await ResumeOn(ui))
                    {
                        co_return;
                    }
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
