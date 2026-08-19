#include <gtest/gtest.h>
#include "../../kernel/mm/page_allocator.hpp"
#include "../../arch/arm/cortex-a/mmu/mmu_manager.hpp"

using namespace auroraos::kernel;
using namespace auroraos::kernel::mmu;

class MmuManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Allocate 2MB for page tables
        static uint8_t pt_memory_pool[2 * 1024 * 1024];
        PageAllocator::instance().init(pt_memory_pool, sizeof(pt_memory_pool));
    }
};

TEST_F(MmuManagerTest, MapAndUnmap) {
    AArch64MmuManager mmu;
    ASSERT_TRUE(mmu.is_valid());

    uintptr_t vaddr = 0x40000000;
    uintptr_t paddr = 0x80000000;

    bool mapped = mmu.map(vaddr, paddr, MapFlags::Read | MapFlags::Write | MapFlags::User);
    EXPECT_TRUE(mapped);
    EXPECT_TRUE(mmu.is_mapped(vaddr));

    // Check if the page is unmapped successfully
    bool unmapped = mmu.unmap(vaddr);
    EXPECT_TRUE(unmapped);
    EXPECT_FALSE(mmu.is_mapped(vaddr));

    // Trying to unmap again should fail
    EXPECT_FALSE(mmu.unmap(vaddr));
}

TEST_F(MmuManagerTest, MapRangeAndUnmapRange) {
    AArch64MmuManager mmu;

    uintptr_t vaddr_base = 0x50000000;
    uintptr_t paddr_base = 0x90000000;
    size_t size = 4 * PageAllocator::PAGE_SIZE; // 4 pages = 16KB

    EXPECT_TRUE(mmu.map_range(vaddr_base, paddr_base, size, MapFlags::Read | MapFlags::Write));

    for (size_t i = 0; i < 4; ++i) {
        uintptr_t v = vaddr_base + i * PageAllocator::PAGE_SIZE;
        uintptr_t p = 0;
        MapFlags flags = MapFlags::Read;
        EXPECT_TRUE(mmu.translate(v, &p, &flags));
        EXPECT_EQ(p, paddr_base + i * PageAllocator::PAGE_SIZE);
        EXPECT_TRUE(flags & MapFlags::Write);
    }

    EXPECT_TRUE(mmu.unmap_range(vaddr_base, size));

    for (size_t i = 0; i < 4; ++i) {
        uintptr_t v = vaddr_base + i * PageAllocator::PAGE_SIZE;
        EXPECT_FALSE(mmu.is_mapped(v));
    }
}

TEST_F(MmuManagerTest, TranslationAndAttributes) {
    AArch64MmuManager mmu;

    uintptr_t v_dev = 0x09000000;
    uintptr_t p_dev = 0x09000000;
    EXPECT_TRUE(mmu.map(v_dev, p_dev, MapFlags::Read | MapFlags::Write | MapFlags::Device));

    uintptr_t v_code = 0x40000000;
    uintptr_t p_code = 0x80000000;
    EXPECT_TRUE(mmu.map(v_code, p_code, MapFlags::Read | MapFlags::Execute | MapFlags::User));

    uintptr_t translated_paddr = 0;
    MapFlags flags = MapFlags::Read;

    // Device check
    EXPECT_TRUE(mmu.translate(v_dev + 0x124, &translated_paddr, &flags));
    EXPECT_EQ(translated_paddr, p_dev + 0x124);
    EXPECT_TRUE(flags & MapFlags::Device);
    EXPECT_TRUE(flags & MapFlags::Write);

    // Code check
    EXPECT_TRUE(mmu.translate(v_code + 0x80, &translated_paddr, &flags));
    EXPECT_EQ(translated_paddr, p_code + 0x80);
    EXPECT_TRUE(flags & MapFlags::Execute);
    EXPECT_TRUE(flags & MapFlags::User);
    EXPECT_FALSE(flags & MapFlags::Write);
}

TEST_F(MmuManagerTest, ProtectPermissionUpdate) {
    AArch64MmuManager mmu;

    uintptr_t vaddr = 0x60000000;
    uintptr_t paddr = 0xA0000000;

    EXPECT_TRUE(mmu.map(vaddr, paddr, MapFlags::Read | MapFlags::Write));

    MapFlags flags = MapFlags::Read;
    EXPECT_TRUE(mmu.translate(vaddr, nullptr, &flags));
    EXPECT_TRUE(flags & MapFlags::Write);

    // Change to Read-Only
    EXPECT_TRUE(mmu.protect(vaddr, MapFlags::Read));
    EXPECT_TRUE(mmu.translate(vaddr, nullptr, &flags));
    EXPECT_FALSE(flags & MapFlags::Write);

    // Protect unmapped address should fail
    EXPECT_FALSE(mmu.protect(0x70000000, MapFlags::Read));
}

TEST_F(MmuManagerTest, AlignmentValidation) {
    AArch64MmuManager mmu;

    // Unaligned vaddr
    EXPECT_FALSE(mmu.map(0x40000001, 0x80000000, MapFlags::Read));
    // Unaligned paddr
    EXPECT_FALSE(mmu.map(0x40000000, 0x80000001, MapFlags::Read));
    // Unaligned unmap
    EXPECT_FALSE(mmu.unmap(0x40000001));
    // Unaligned protect
    EXPECT_FALSE(mmu.protect(0x40000001, MapFlags::Read));
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

TEST_F(MmuManagerTest, MoveSemanticsTransferOwnership) {
    size_t initial_free = PageAllocator::instance().get_free_pages();
    {
        AArch64MmuManager mmu1;
        mmu1.map(0x40000000, 0x80000000, MapFlags::Read | MapFlags::Write);
        uintptr_t pgdir = mmu1.get_pgdir_base();

        AArch64MmuManager mmu2(std::move(mmu1));
        EXPECT_EQ(mmu1.get_pgdir_base(), 0u);
        EXPECT_EQ(mmu2.get_pgdir_base(), pgdir);
        EXPECT_TRUE(mmu2.is_mapped(0x40000000));
    }
    EXPECT_EQ(PageAllocator::instance().get_free_pages(), initial_free);
}

TEST_F(MmuManagerTest, EmptyTablePruningOnUnmap) {
    size_t initial_free = PageAllocator::instance().get_free_pages();
    {
        AArch64MmuManager mmu;
        mmu.map(0x40000000, 0x80000000, MapFlags::Read | MapFlags::Write);
        EXPECT_TRUE(mmu.unmap(0x40000000));
    }
    // When the single mapped page is unmapped, all intermediate tables are pruned,
    // and when mmu destructs, L0 is freed as well -> back to initial_free.
    EXPECT_EQ(PageAllocator::instance().get_free_pages(), initial_free);
}

TEST_F(MmuManagerTest, MultipleDisjointMappings) {
    size_t initial_free = PageAllocator::instance().get_free_pages();
    {
        AArch64MmuManager mmu;
        // Distinct L0 / L1 / L2 branches
        EXPECT_TRUE(mmu.map(0x00001000, 0x10001000, MapFlags::Read));
        EXPECT_TRUE(mmu.map(0x40000000, 0x80000000, MapFlags::Read | MapFlags::Write));
        EXPECT_TRUE(mmu.map(0x80000000, 0xC0000000, MapFlags::Read | MapFlags::Device));
        EXPECT_TRUE(mmu.map(0x00007FFFF000ULL, 0x20000000, MapFlags::Read | MapFlags::User));

        EXPECT_TRUE(mmu.is_mapped(0x00001000));
        EXPECT_TRUE(mmu.is_mapped(0x40000000));
        EXPECT_TRUE(mmu.is_mapped(0x80000000));
        EXPECT_TRUE(mmu.is_mapped(0x00007FFFF000ULL));
    }
    EXPECT_EQ(PageAllocator::instance().get_free_pages(), initial_free);
}

TEST_F(MmuManagerTest, MapKernelRegions) {
    size_t initial_free = PageAllocator::instance().get_free_pages();
    {
        AArch64MmuManager mmu;
        EXPECT_TRUE(mmu.map_kernel_regions());

        // 1. Check Kernel RAM at base (0x40000000)
        uintptr_t paddr = 0;
        MapFlags flags = MapFlags::None;
        EXPECT_TRUE(mmu.translate(0x40000000, &paddr, &flags));
        EXPECT_EQ(paddr, 0x40000000u);
        EXPECT_TRUE(flags & MapFlags::Read);
        EXPECT_TRUE(flags & MapFlags::Write);
        EXPECT_TRUE(flags & MapFlags::Execute);
        EXPECT_FALSE(flags & MapFlags::User); // Privileged only

        // 2. Check Kernel RAM near end (0x47FFF000)
        EXPECT_TRUE(mmu.translate(0x47FFF000, &paddr, &flags));
        EXPECT_EQ(paddr, 0x47FFF000u);
        EXPECT_TRUE(flags & MapFlags::Execute);
        EXPECT_FALSE(flags & MapFlags::User);

        // 3. Check UART MMIO (0x09000000)
        EXPECT_TRUE(mmu.translate(0x09000000, &paddr, &flags));
        EXPECT_EQ(paddr, 0x09000000u);
        EXPECT_TRUE(flags & MapFlags::Device);
        EXPECT_FALSE(flags & MapFlags::User);

        // 4. Check GIC MMIO (0x08000000)
        EXPECT_TRUE(mmu.translate(0x08000000, &paddr, &flags));
        EXPECT_EQ(paddr, 0x08000000u);
        EXPECT_TRUE(flags & MapFlags::Device);
        EXPECT_FALSE(flags & MapFlags::User);
    }
    // All page tables allocated for kernel mappings must be cleanly freed on destruction
    EXPECT_EQ(PageAllocator::instance().get_free_pages(), initial_free);
}

TEST_F(MmuManagerTest, KernelAndUserCoexistence) {
    size_t initial_free = PageAllocator::instance().get_free_pages();
    {
        AArch64MmuManager mmu;
        EXPECT_TRUE(mmu.map_kernel_regions());

        // Map user application code and data pages
        uintptr_t user_app_va = 0x00400000;
        uintptr_t user_app_pa = 0x45000000;
        EXPECT_TRUE(mmu.map(user_app_va, user_app_pa,
                            MapFlags::Read | MapFlags::Execute | MapFlags::User));

        uintptr_t user_stack_va = 0x00800000;
        uintptr_t user_stack_pa = 0x45001000;
        EXPECT_TRUE(mmu.map(user_stack_va, user_stack_pa,
                            MapFlags::Read | MapFlags::Write | MapFlags::User));

        // Verify user pages have MapFlags::User
        uintptr_t paddr = 0;
        MapFlags flags = MapFlags::None;
        EXPECT_TRUE(mmu.translate(user_app_va, &paddr, &flags));
        EXPECT_EQ(paddr, user_app_pa);
        EXPECT_TRUE(flags & MapFlags::User);
        EXPECT_TRUE(flags & MapFlags::Execute);

        EXPECT_TRUE(mmu.translate(user_stack_va, &paddr, &flags));
        EXPECT_EQ(paddr, user_stack_pa);
        EXPECT_TRUE(flags & MapFlags::User);
        EXPECT_TRUE(flags & MapFlags::Write);

        // Verify kernel space in the same VAS is NOT accessible by User (EL0)
        EXPECT_TRUE(mmu.translate(0x40000000, &paddr, &flags));
        EXPECT_FALSE(flags & MapFlags::User);
    }
    EXPECT_EQ(PageAllocator::instance().get_free_pages(), initial_free);
}

