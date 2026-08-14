#include <gtest/gtest.h>
#include "../../kernel/mm/page_allocator.hpp"
#include "../../arch/arm/cortex-a/mmu/mmu_manager.hpp"

using namespace auroraos::kernel;
using namespace auroraos::kernel::mmu;

class MmuManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Allocate 1MB for page tables
        static uint8_t pt_memory_pool[1024 * 1024];
        PageAllocator::instance().init(pt_memory_pool, sizeof(pt_memory_pool));
    }
};

TEST_F(MmuManagerTest, MapAndUnmap) {
    AArch64MmuManager mmu;

    uintptr_t vaddr = 0x40000000;
    uintptr_t paddr = 0x80000000;

    bool mapped = mmu.map(vaddr, paddr, MapFlags::Read | MapFlags::Write | MapFlags::User);
    EXPECT_TRUE(mapped);

    // Check if the page is unmapped successfully
    bool unmapped = mmu.unmap(vaddr);
    EXPECT_TRUE(unmapped);

    // Trying to unmap again should fail
    EXPECT_FALSE(mmu.unmap(vaddr));
}

TEST_F(MmuManagerTest, DestructorRecursiveFree) {
    size_t initial_free = PageAllocator::instance().get_free_pages();
    {
        AArch64MmuManager mmu;
        // Map pages that require allocating L1, L2, L3 tables
        mmu.map(0x40000000, 0x80000000, MapFlags::Read | MapFlags::Write);
        mmu.map(0x40001000, 0x80001000, MapFlags::Read | MapFlags::Write);

        EXPECT_LT(PageAllocator::instance().get_free_pages(), initial_free);
    }
    // After mmu goes out of scope, the destructor should free all allocated page tables
    EXPECT_EQ(PageAllocator::instance().get_free_pages(), initial_free);
}
