#include "pch.h"
#include "Modes/ScriptRunner.h"

#include <string>
#include <vector>

#include "Diagnostics/Log.h"
#include "Diagnostics/PerfLog.h"
#include "Modes/Modes.h"
#include "Text/Utf.h"
#include "Util/Async.h"
#include "ViewerHost.h"

namespace fastlook::viewer
{
    namespace
    {
        std::vector<std::wstring> SplitCommands(std::wstring_view script)
        {
            std::vector<std::wstring> commands;
            while (!script.empty())
            {
                auto const end = script.find(L';');
                std::wstring_view part = script.substr(0, end);
                while (!part.empty() && iswspace(part.front()))
                {
                    part.remove_prefix(1);
                }
                while (!part.empty() && iswspace(part.back()))
                {
                    part.remove_suffix(1);
                }
                if (!part.empty())
                {
                    commands.emplace_back(part);
                }
                script = end == std::wstring_view::npos ? std::wstring_view{} : script.substr(end + 1);
            }
            return commands;
        }

        // Runs one command; returns the delay to wait afterwards (0 = none).
        uint32_t RunCommand(std::wstring_view command)
        {
            auto& host = ViewerHost::Get();
            auto const colon = command.find(L':');
            std::wstring_view const verb = command.substr(0, colon);
            std::wstring_view const arg = colon == std::wstring_view::npos ? std::wstring_view{} : command.substr(colon + 1);
            core::perf::Mark("script_" + core::ToUtf8(verb), core::ToUtf8(arg));

            if (verb == L"wait")
            {
                return static_cast<uint32_t>(std::wcstoul(std::wstring{arg}.c_str(), nullptr, 10));
            }
            if (verb == L"show")
            {
                host.Show(PayloadFromArgs(host.Args()));
            }
            else if (verb == L"next")
            {
                host.Execute(Command::Next);
            }
            else if (verb == L"prev")
            {
                host.Execute(Command::Prev);
            }
            else if (verb == L"zoomin")
            {
                host.Execute(Command::ZoomIn);
            }
            else if (verb == L"zoomout")
            {
                host.Execute(Command::ZoomOut);
            }
            else if (verb == L"zoomreset")
            {
                host.Execute(Command::ZoomReset);
            }
            else if (verb == L"fullscreen")
            {
                host.Execute(Command::ToggleFullscreen);
            }
            else if (verb == L"pin")
            {
                host.Execute(Command::Pin);
            }
            else if (verb == L"close")
            {
                host.Execute(Command::Close);
            }
            else if (verb == L"open")
            {
                host.Execute(Command::OpenDefault);
            }
            else if (verb == L"copy")
            {
                host.Execute(Command::Copy);
            }
            else if (verb == L"cmd")
            {
                if (!host.ExecutePreviewerCommand(arg))
                {
                    FL_LOG_INFO(L"Script: previewer command not handled");
                }
            }
            else
            {
                FL_LOG_INFO(L"Script: unknown command ignored");
            }
            return 0;
        }

        winrt::fire_and_forget RunScriptAsync(std::wstring script)
        {
            auto const ui = ViewerHost::Get().Ui();
            try
            {
                for (auto const& command : SplitCommands(script))
                {
                    if (uint32_t const delayMs = RunCommand(command))
                    {
                        co_await winrt::resume_after(std::chrono::milliseconds(delayMs));
                        if (!co_await ResumeOn(ui))
                        {
                            co_return;
                        }
                    }
                }
            }
            catch (...)
            {
                FL_LOG_INFO(L"Script aborted by an exception");
            }
        }
    }

    void RunScript(std::wstring_view script)
    {
        RunScriptAsync(std::wstring{script});
    }
}
