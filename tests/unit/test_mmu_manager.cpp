#include <gtest/gtest.h>
#include "../../kernel/page_allocator.hpp"
#include "../../arch/arm/cortex-a/mmu/mmu_manager.hpp"

using namespace auroraos::kernel;
using namespace auroraos::kernel::mmu;

TEST(MmuManagerTest, MapAndUnmap) {
    // Allocate 1MB for page tables
    alignas(4096) static uint8_t pt_memory_pool[1024 * 1024];
    PageAllocator::instance().init(pt_memory_pool, sizeof(pt_memory_pool));
    
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
