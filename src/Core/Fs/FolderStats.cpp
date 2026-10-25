#include "Fs/FolderStats.h"

#include <Windows.h>

#include <string>
#include <vector>

namespace fastlook::core
{
    namespace
    {
        constexpr auto kProgressInterval = std::chrono::milliseconds(100);
        constexpr unsigned kDeadlineCheckMask = 63; // clock read every 64 entries
        // Reparse points are never followed; cloud directories (listing them would populate them) are counted, not entered.
        constexpr DWORD kNoDescend =
            FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_OFFLINE;

        // Paths near MAX_PATH need the extended-length prefix.
        std::wstring SearchPattern(std::wstring dir)
        {
            if (dir.size() >= 240 && !dir.starts_with(L"\\\\?\\"))
            {
                if (dir.starts_with(L"\\\\"))
                {
                    dir = L"\\\\?\\UNC\\" + dir.substr(2);
                }
                else
                {
                    dir = L"\\\\?\\" + dir;
                }
            }
            if (!dir.empty() && dir.back() != L'\\')
            {
                dir += L'\\';
            }
            return dir + L'*';
        }
    }

    FolderStats CountFolder(std::filesystem::path const& root, std::stop_token stop, std::chrono::steady_clock::time_point deadline,
                            std::function<void(FolderStats const&)> progress)
    {
        using clock = std::chrono::steady_clock;
        FolderStats stats;
        auto lastProgress = clock::now();
        unsigned seen = 0;

        std::vector<std::wstring> pending;
        pending.push_back(root.wstring());

        while (!pending.empty())
        {
            std::wstring const dir = std::move(pending.back());
            pending.pop_back();

            WIN32_FIND_DATAW data;
            HANDLE const find = FindFirstFileExW(SearchPattern(dir).c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                                 FIND_FIRST_EX_LARGE_FETCH);
            if (find == INVALID_HANDLE_VALUE)
            {
                if (stop.stop_requested() || clock::now() >= deadline)
                {
                    stats.truncated = true;
                    break;
                }
                continue;
            }

            bool done = false;
            do
            {
                if ((seen++ & kDeadlineCheckMask) == 0 || stop.stop_requested())
                {
                    auto const now = clock::now();
                    if (stop.stop_requested() || now >= deadline)
                    {
                        stats.truncated = true;
                        done = true;
                        break;
                    }
                    if (progress && now - lastProgress >= kProgressInterval)
                    {
                        lastProgress = now;
                        progress(stats);
                    }
                }

                std::wstring_view const name = data.cFileName;
                if (name == L"." || name == L"..")
                {
                    continue;
                }
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    ++stats.folders;
                    if (!(data.dwFileAttributes & kNoDescend))
                    {
                        pending.push_back(dir + (dir.ends_with(L'\\') ? L"" : L"\\") + data.cFileName);
                    }
                }
                else
                {
                    ++stats.files;
                    stats.bytes += (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                }
            } while (FindNextFileW(find, &data));
            FindClose(find);

            if (done)
            {
                break;
            }
        }

        if (progress)
        {
            progress(stats);
        }
        return stats;
    }
}
