#include "FileTypes/ClassifyFile.h"

#include <Windows.h>

#include <vector>

#include "FileTypes/Extensions.h"
#include "FileTypes/FileName.h"
#include "FileTypes/Sniff.h"
#include "Shell/HandlerLookup.h"

namespace fastlook::core
{
    namespace
    {
        constexpr size_t kHeadSize = 4096;
        constexpr DWORD kPlaceholderAttributes =
            FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE;

        // Win32 file APIs stop at MAX_PATH without the extended-length prefix.
        std::wstring ExtendedPath(std::wstring_view path)
        {
            if (path.size() < MAX_PATH || path.starts_with(L"\\\\?\\"))
                return std::wstring(path);
            if (path.starts_with(L"\\\\"))
                return L"\\\\?\\UNC\\" + std::wstring(path.substr(2));
            return L"\\\\?\\" + std::wstring(path);
        }

        bool ReadHead(std::wstring const& path, std::vector<uint8_t>& head)
        {
            HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;
            head.resize(kHeadSize);
            DWORD read = 0;
            bool ok = ReadFile(file, head.data(), static_cast<DWORD>(head.size()), &read, nullptr) != FALSE;
            CloseHandle(file);
            head.resize(ok ? read : 0);
            return ok;
        }
    }

    FileClassification ClassifyFile(std::wstring_view path, bool isVirtual, bool useShellHandlers)
    {
        FileClassification result;
        if (isVirtual || path.empty())
            return result;

        std::wstring fullPath = ExtendedPath(path);
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(fullPath.c_str(), GetFileExInfoStandard, &data))
            return result;

        if (data.dwFileAttributes & kPlaceholderAttributes)
        {
            result.cloudPlaceholder = true; // never open it: that would hydrate the file
            return result;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            result.kind = PreviewerKind::Folder;
            return result;
        }

        std::vector<uint8_t> head;
        if (!ReadHead(fullPath, head))
            return result;

        ClassifyInput in;
        in.fileName = path;
        in.head = head;

        auto byExt = KindForExtension(path);
        bool decidedByTable = byExt && *byExt != PreviewerKind::Text && *byExt != PreviewerKind::Markdown;
        std::wstring ext = LowerExtension(path);
        if (!decidedByTable && !ext.empty())
        {
            in.perceived = GetPerceivedKind(ext);
            in.hasWicDecoder = HasWicDecoder(ext);
        }

        in.useShellHandlers = false; // first pass: everything except shell handlers
        result.kind = ClassifyPreview(in);
        if (result.kind != PreviewerKind::Fallback || !useShellHandlers || ext.empty())
            return result;

        auto clsid = FindPreviewHandlerClsid(ext);
        if (!clsid || IsBlockedPreviewHandler(*clsid, HandlerServerPath(*clsid)))
            return result;
        in.hasShellHandler = true;
        in.useShellHandlers = true;
        result.kind = ClassifyPreview(in);
        if (result.kind == PreviewerKind::Shell)
            result.shellHandlerClsid = std::move(clsid);
        return result;
    }
}
