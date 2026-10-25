#include <Windows.h>

#include "Peek/PeekDetect.h"
#include "TestUtil.h"

using namespace fastlook::core;

namespace
{
    // Sets an environment variable for the scope and restores the previous state.
    class ScopedEnv
    {
    public:
        ScopedEnv(wchar_t const* name, wchar_t const* value) : m_name(name)
        {
            wchar_t buf[1024];
            DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
            if (n > 0 && n < std::size(buf))
            {
                m_old = buf;
                m_had = true;
            }
            SetEnvironmentVariableW(name, value);
        }
        ~ScopedEnv()
        {
            SetEnvironmentVariableW(m_name.c_str(), m_had ? m_old.c_str() : nullptr);
        }

    private:
        std::wstring m_name, m_old;
        bool m_had = false;
    };
}

TEST(PeekDetect, Cases)
{
    auto on = Fixture("peek/global-enabled.json"), off = Fixture("peek/global-disabled.json");
    EXPECT_TRUE(DetectPeekConflict(on, Fixture("peek/peek-space.json"), ActivationKey::Space).conflict);
    EXPECT_FALSE(DetectPeekConflict(off, Fixture("peek/peek-space.json"), ActivationKey::Space).conflict);
    EXPECT_FALSE(DetectPeekConflict(on, Fixture("peek/peek-ctrl-space.json"), ActivationKey::Space).conflict);
    EXPECT_TRUE(DetectPeekConflict(on, Fixture("peek/peek-ctrl-space.json"), ActivationKey::CtrlSpace).conflict);
    EXPECT_TRUE(DetectPeekConflict(on, Fixture("peek/peek-ctrl-space-plus-space-to-activate.json"), ActivationKey::Space).conflict);
    EXPECT_TRUE(DetectPeekConflict(on, "", ActivationKey::CtrlSpace).conflict); // missing Peek file -> default Ctrl+Space
    EXPECT_FALSE(DetectPeekConflict(Fixture("peek/malformed.json"), "", ActivationKey::Space).peekEnabled);
}

TEST(PeekDetect, EnabledFlagIsReported)
{
    auto on = Fixture("peek/global-enabled.json"), off = Fixture("peek/global-disabled.json");
    EXPECT_TRUE(DetectPeekConflict(on, Fixture("peek/peek-ctrl-space.json"), ActivationKey::Space).peekEnabled);
    EXPECT_FALSE(DetectPeekConflict(off, Fixture("peek/peek-ctrl-space.json"), ActivationKey::CtrlSpace).peekEnabled);
}

TEST(PeekDetect, SpaceToActivateConflictsOnlyWithPlainSpace)
{
    auto on = Fixture("peek/global-enabled.json");
    auto p = Fixture("peek/peek-ctrl-space-plus-space-to-activate.json");
    EXPECT_FALSE(DetectPeekConflict(on, p, ActivationKey::AltSpace).conflict);
    EXPECT_FALSE(DetectPeekConflict(on, p, ActivationKey::ShiftSpace).conflict);
    EXPECT_TRUE(DetectPeekConflict(on, p, ActivationKey::CtrlSpace).conflict);
}

TEST(PeekDetect, ActivationShortcutBeatsDefault)
{
    // peek-space.json: configured shortcut is plain Space (default Ctrl+Space is overridden).
    auto on = Fixture("peek/global-enabled.json");
    auto p = Fixture("peek/peek-space.json");
    EXPECT_FALSE(DetectPeekConflict(on, p, ActivationKey::CtrlSpace).conflict);
}

TEST(PeekDetect, FallsBackToDefaultShortcutWhenActivationMissing)
{
    auto on = Fixture("peek/global-enabled.json");
    std::string p = R"({"properties":{"DefaultActivationShortcut":{"win":false,"ctrl":false,"alt":true,"shift":false,"code":32}}})";
    EXPECT_TRUE(DetectPeekConflict(on, p, ActivationKey::AltSpace).conflict);
    EXPECT_FALSE(DetectPeekConflict(on, p, ActivationKey::CtrlSpace).conflict);
}

TEST(PeekDetect, WinShortcutNeverMatchesOurs)
{
    auto on = Fixture("peek/global-enabled.json");
    std::string p = R"({"properties":{"ActivationShortcut":{"win":true,"ctrl":false,"alt":false,"shift":false,"code":32}}})";
    for (auto k : {ActivationKey::Space, ActivationKey::CtrlSpace, ActivationKey::AltSpace, ActivationKey::ShiftSpace})
    {
        EXPECT_FALSE(DetectPeekConflict(on, p, k).conflict);
    }
}

TEST(PeekDetect, OtherKeyCodeIsNoConflict)
{
    auto on = Fixture("peek/global-enabled.json");
    std::string p = R"({"properties":{"ActivationShortcut":{"win":false,"ctrl":true,"alt":false,"shift":false,"code":81}}})";
    EXPECT_FALSE(DetectPeekConflict(on, p, ActivationKey::CtrlSpace).conflict);
}

TEST(PeekDetect, MalformedPeekFileFallsBackToDefault)
{
    auto on = Fixture("peek/global-enabled.json");
    auto bad = Fixture("peek/malformed.json");
    auto r = DetectPeekConflict(on, bad, ActivationKey::CtrlSpace);
    EXPECT_TRUE(r.peekEnabled);
    EXPECT_TRUE(r.conflict);
    EXPECT_FALSE(DetectPeekConflict(on, bad, ActivationKey::Space).conflict);
}

TEST(PeekDetect, WrongTypesNeverThrow)
{
    auto on = Fixture("peek/global-enabled.json");
    for (std::string_view p : {"[]", "null", "42", R"({"properties":5})", R"({"properties":{"ActivationShortcut":"x"}})",
                               R"({"properties":{"ActivationShortcut":{"code":"32"}}})", R"({"properties":{"EnableSpaceToActivate":true}})"})
    {
        EXPECT_NO_THROW(DetectPeekConflict(on, p, ActivationKey::Space));
    }
    for (std::string_view g : {"", "[]", "null", R"({"enabled":5})", R"({"enabled":{"Peek":"true"}})", R"({"enabled":{}})"})
    {
        auto r = DetectPeekConflict(g, "", ActivationKey::CtrlSpace);
        EXPECT_FALSE(r.peekEnabled);
        EXPECT_FALSE(r.conflict);
    }
}

TEST(PeekDetect, OnDiskReadsFixtureDir)
{
    auto dir = MakeTempDir();
    std::filesystem::create_directories(dir / L"Peek");
    WriteFile(dir / L"settings.json", Fixture("peek/global-enabled.json"));
    WriteFile(dir / L"Peek" / L"settings.json", Fixture("peek/peek-space.json"));
    ScopedEnv env(L"FASTLOOK_POWERTOYS_DIR", dir.c_str());
    EXPECT_TRUE(DetectPeekConflictOnDisk(ActivationKey::Space).conflict);
    EXPECT_FALSE(DetectPeekConflictOnDisk(ActivationKey::CtrlSpace).conflict);
    EXPECT_TRUE(DetectPeekConflictOnDisk(ActivationKey::Space).peekEnabled);
}

TEST(PeekDetect, OnDiskMissingFilesIsNoPeek)
{
    auto dir = MakeTempDir();
    ScopedEnv env(L"FASTLOOK_POWERTOYS_DIR", dir.c_str());
    auto r = DetectPeekConflictOnDisk(ActivationKey::Space);
    EXPECT_FALSE(r.peekEnabled);
    EXPECT_FALSE(r.conflict);
}

TEST(PeekDetect, OnDiskMissingPeekFileUsesDefaultShortcut)
{
    auto dir = MakeTempDir();
    WriteFile(dir / L"settings.json", Fixture("peek/global-enabled.json"));
    ScopedEnv env(L"FASTLOOK_POWERTOYS_DIR", dir.c_str());
    EXPECT_TRUE(DetectPeekConflictOnDisk(ActivationKey::CtrlSpace).conflict);
    EXPECT_FALSE(DetectPeekConflictOnDisk(ActivationKey::Space).conflict);
}

TEST(PeekDetect, OnDiskMalformedGlobalIsNoPeek)
{
    auto dir = MakeTempDir();
    WriteFile(dir / L"settings.json", Fixture("peek/malformed.json"));
    ScopedEnv env(L"FASTLOOK_POWERTOYS_DIR", dir.c_str());
    EXPECT_FALSE(DetectPeekConflictOnDisk(ActivationKey::Space).peekEnabled);
}

TEST(PeekDetect, OnDiskDoesNotModifyFiles)
{
    auto dir = MakeTempDir();
    std::filesystem::create_directories(dir / L"Peek");
    auto g = Fixture("peek/global-enabled.json"), p = Fixture("peek/peek-space.json");
    WriteFile(dir / L"settings.json", g);
    WriteFile(dir / L"Peek" / L"settings.json", p);
    ScopedEnv env(L"FASTLOOK_POWERTOYS_DIR", dir.c_str());
    DetectPeekConflictOnDisk(ActivationKey::Space);
    EXPECT_EQ(ReadFile(dir / L"settings.json"), g);
    EXPECT_EQ(ReadFile(dir / L"Peek" / L"settings.json"), p);
}
