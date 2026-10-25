#include <gtest/gtest.h>

#include <Windows.h>
#include <objbase.h>

#include <string>

#include "FileTypes/ClassifyFile.h"
#include "TestUtil.h"

using namespace fastlook::core;

namespace
{
    struct CoInit
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ~CoInit()
        {
            if (SUCCEEDED(hr))
                CoUninitialize();
        }
    };

    std::wstring WriteFile(std::filesystem::path const& dir, wchar_t const* name, std::string const& bytes)
    {
        auto p = dir / name;
        std::ofstream(p, std::ios::binary) << bytes;
        return p.wstring();
    }
}

TEST(ClassifyFile, RealFiles)
{
    CoInit com;
    auto dir = MakeTempDir();
    EXPECT_EQ(ClassifyFile(dir.wstring(), false, true).kind, PreviewerKind::Folder);
    EXPECT_EQ(ClassifyFile(WriteFile(dir, L"a.cpp", "int x;\n"), false, true).kind, PreviewerKind::Text);
    EXPECT_EQ(ClassifyFile(WriteFile(dir, L"empty.txt", ""), false, true).kind, PreviewerKind::Text);
    EXPECT_EQ(ClassifyFile(WriteFile(dir, L"a.png", "not really"), false, true).kind, PreviewerKind::Image);
    EXPECT_EQ(ClassifyFile(WriteFile(dir, L"bin.txt", std::string("ab\0cd", 5)), false, false).kind, PreviewerKind::Fallback);
    EXPECT_EQ(ClassifyFile(WriteFile(dir, L"noext", "hello\n"), false, true).kind, PreviewerKind::Text);
}

TEST(ClassifyFile, MissingVirtualAndPlaceholder)
{
    CoInit com;
    auto dir = MakeTempDir();
    EXPECT_EQ(ClassifyFile((dir / L"nope.txt").wstring(), false, true).kind, PreviewerKind::Fallback);
    EXPECT_EQ(ClassifyFile(L"This PC", true, true).kind, PreviewerKind::Fallback);

    auto path = WriteFile(dir, L"cloud.txt", "hello");
    ASSERT_TRUE(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_OFFLINE));
    // An exclusive lock makes any open attempt fail: the placeholder must be recognised from attributes alone.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(h, INVALID_HANDLE_VALUE);
    auto r = ClassifyFile(path, false, true);
    CloseHandle(h);
    EXPECT_EQ(r.kind, PreviewerKind::Fallback);
    EXPECT_TRUE(r.cloudPlaceholder);
}

TEST(ClassifyFile, LockedExclusivelyIsFallback)
{
    CoInit com;
    auto dir = MakeTempDir();
    auto path = WriteFile(dir, L"locked.cpp", "int x;");
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(h, INVALID_HANDLE_VALUE);
    EXPECT_EQ(ClassifyFile(path, false, true).kind, PreviewerKind::Fallback);
    CloseHandle(h);
}

TEST(ClassifyFile, TableDecidedKindsDoNotOpenTheFile)
{
    CoInit com;
    auto dir = MakeTempDir();
    for (auto [name, kind] : {std::pair{L"locked.png", PreviewerKind::Image}, std::pair{L"locked.mp4", PreviewerKind::Media},
                              std::pair{L"locked.pdf", PreviewerKind::Pdf}, std::pair{L"locked.zip", PreviewerKind::Archive}})
    {
        auto path = WriteFile(dir, name, "x");
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(h, INVALID_HANDLE_VALUE);
        EXPECT_EQ(ClassifyFile(path, false, true).kind, kind);
        CloseHandle(h);
    }
}
