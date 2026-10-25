#include <Windows.h>

#include <stdexcept>

#include "Common/SafeMem.h"
#include "TestUtil.h"

using namespace fastlook::core;

namespace
{
    // A committed page that can later be made inaccessible.
    struct Page
    {
        uint8_t* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        ~Page() { VirtualFree(p, 0, MEM_RELEASE); }
        void Poison() const
        {
            DWORD old;
            VirtualProtect(p, 4096, PAGE_NOACCESS, &old);
        }
    };
}

TEST(SafeMem, CopiesReadableMemory)
{
    char src[] = "hello", dst[6] = {};
    EXPECT_TRUE(SafeCopy(dst, src, sizeof src));
    EXPECT_STREQ(dst, "hello");
}

TEST(SafeMem, FaultingSourceReturnsFalse)
{
    Page page;
    ASSERT_NE(page.p, nullptr);
    page.Poison();
    char dst[16];
    EXPECT_FALSE(SafeCopy(dst, page.p, sizeof dst));
}

TEST(SafeMem, CppExceptionsPropagate)
{
    int dummy = 0;
    EXPECT_THROW(SafeRun(&dummy, sizeof dummy, [] { throw std::runtime_error("x"); }), std::runtime_error);
}
