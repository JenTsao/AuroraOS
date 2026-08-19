// =============================================================================
// tests/unit/test_mmu_subsystem.cpp
//
// 统一 MMU 子系统与多任务虚拟地址空间隔离全流程测试
// =============================================================================
#include <gtest/gtest.h>
#include <cstring>

#include "../../kernel/mm/mmu.hpp"
#include "../../kernel/task/task.hpp"

using namespace auroraos::kernel;

class MmuSubsystemTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 分配 4MB 物理页内存池
        static uint8_t pt_memory_pool[4 * 1024 * 1024];
        Mmu::instance().init(pt_memory_pool, sizeof(pt_memory_pool));
    }
};

// =============================================================================
// 1. MMU 子系统初始化与内核常驻空间测试
// =============================================================================
TEST_F(MmuSubsystemTest, InitAndKernelVas) {
    Mmu& mmu = Mmu::instance();
    EXPECT_TRUE(mmu.is_mmu_supported());

    VirtualAddressSpace* kernel_vas = mmu.get_kernel_vas();
    ASSERT_NE(kernel_vas, nullptr);
    EXPECT_NE(kernel_vas->get_pgdir_base(), 0u);

    // 验证内核关键区域已常驻映射（特权级访问，User 位为 0）
    uintptr_t paddr = 0;
    MapFlags flags = MapFlags::None;

    // 1. 内核 RAM
    EXPECT_TRUE(kernel_vas->translate(KERNEL_RAM_BASE, &paddr, &flags));
    EXPECT_EQ(paddr, KERNEL_RAM_BASE);
    EXPECT_TRUE(flags & MapFlags::Read);
    EXPECT_TRUE(flags & MapFlags::Write);
    EXPECT_TRUE(flags & MapFlags::Execute);
    EXPECT_FALSE(flags & MapFlags::User);

    // 2. UART MMIO (0x09000000)
    EXPECT_TRUE(kernel_vas->translate(0x09000000, &paddr, &flags));
    EXPECT_EQ(paddr, 0x09000000u);
    EXPECT_TRUE(flags & MapFlags::Device);
    EXPECT_FALSE(flags & MapFlags::User);
}

// =============================================================================
// 2. 任务虚拟地址空间分配、用户内存映射与销毁测试
// =============================================================================
TEST_F(MmuSubsystemTest, TaskVasLifecycleAndUserMapping) {
    Mmu& mmu = Mmu::instance();
    size_t initial_free = mmu.get_free_pages();

    TaskControlBlock task{};
    task.scheduler.id = 1;

    // 1. 为任务分配独立 VAS
    EXPECT_TRUE(mmu.create_task_vas(&task));
    ASSERT_NE(task.memory.vasp, nullptr);
    EXPECT_NE(task.memory.pgdir_base, 0u);
    EXPECT_LT(mmu.get_free_pages(), initial_free);

    // 2. 验证任务 VAS 继承了内核常驻映射
    uintptr_t paddr = 0;
    MapFlags flags = MapFlags::None;
    EXPECT_TRUE(mmu.translate(&task, KERNEL_RAM_BASE, &paddr, &flags));
    EXPECT_FALSE(flags & MapFlags::User);

    // 3. 映射用户代码段与数据段
    uintptr_t user_code_va = USER_SPACE_BASE;
    uintptr_t user_code_pa = 0x50000000;
    size_t code_size = 2 * MMU_PAGE_SIZE; // 8KB

    EXPECT_TRUE(mmu.map_user_memory(&task, user_code_va, user_code_pa, code_size,
                                    MapFlags::Read | MapFlags::Execute));

    // 4. 验证用户空间映射属性
    EXPECT_TRUE(mmu.translate(&task, user_code_va, &paddr, &flags));
    EXPECT_EQ(paddr, user_code_pa);
    EXPECT_TRUE(flags & MapFlags::User);
    EXPECT_TRUE(flags & MapFlags::Execute);
    EXPECT_FALSE(flags & MapFlags::Write);

    // 5. 动态修改权限为读写 (RW)
    EXPECT_TRUE(mmu.protect_user_memory(&task, user_code_va, code_size,
                                        MapFlags::Read | MapFlags::Write));
    EXPECT_TRUE(mmu.translate(&task, user_code_va, &paddr, &flags));
    EXPECT_TRUE(flags & MapFlags::Write);

    // 6. 撤销映射
    EXPECT_TRUE(mmu.unmap_user_memory(&task, user_code_va, code_size));
    EXPECT_FALSE(mmu.translate(&task, user_code_va, &paddr, &flags));

    // 7. 销毁任务 VAS 并回收全部页表
    mmu.destroy_task_vas(&task);
    EXPECT_EQ(task.memory.vasp, nullptr);
    EXPECT_EQ(task.memory.pgdir_base, 0u);

    // 验证物理页完全回收，无内存泄漏
    EXPECT_EQ(mmu.get_free_pages(), initial_free);
}

// =============================================================================
// 3. 多任务地址空间隔离测试 (Process Address Space Isolation)
// =============================================================================
TEST_F(MmuSubsystemTest, MultiTaskAddressSpaceIsolation) {
    Mmu& mmu = Mmu::instance();
    size_t initial_free = mmu.get_free_pages();

    TaskControlBlock task1{};
    task1.scheduler.id = 1;

    TaskControlBlock task2{};
    task2.scheduler.id = 2;

    EXPECT_TRUE(mmu.create_task_vas(&task1));
    EXPECT_TRUE(mmu.create_task_vas(&task2));

    // 两个任务拥有不同的页表基址 (TTBR0)
    EXPECT_NE(task1.memory.pgdir_base, task2.memory.pgdir_base);

    // 两个任务在同一虚拟地址 (0x00400000) 映射不同的物理地址
    uintptr_t same_va = USER_SPACE_BASE;
    uintptr_t pa1 = 0x60000000;
    uintptr_t pa2 = 0x70000000;

    EXPECT_TRUE(mmu.map_user_memory(&task1, same_va, pa1, MMU_PAGE_SIZE, MapFlags::Read | MapFlags::Write));
    EXPECT_TRUE(mmu.map_user_memory(&task2, same_va, pa2, MMU_PAGE_SIZE, MapFlags::Read | MapFlags::Write));

    // 验证 Task1 转换结果为 PA1
    uintptr_t translated_pa = 0;
    EXPECT_TRUE(mmu.translate(&task1, same_va, &translated_pa));
    EXPECT_EQ(translated_pa, pa1);

    // 验证 Task2 转换结果为 PA2
    EXPECT_TRUE(mmu.translate(&task2, same_va, &translated_pa));
    EXPECT_EQ(translated_pa, pa2);

    // 销毁 Task1，不应影响 Task2 的映射
    mmu.destroy_task_vas(&task1);
    EXPECT_TRUE(mmu.translate(&task2, same_va, &translated_pa));
    EXPECT_EQ(translated_pa, pa2);

    // 销毁 Task2
    mmu.destroy_task_vas(&task2);
    EXPECT_EQ(mmu.get_free_pages(), initial_free);
}

// =============================================================================
// 4. 调度器回收任务与 MMU 空间析构联动测试
// =============================================================================
TEST_F(MmuSubsystemTest, SchedulerFreeTaskCleansUpVas) {
    Mmu& mmu = Mmu::instance();
    size_t initial_free = mmu.get_free_pages();

    static uint32_t stack_space[256];
    TaskControlBlock* task = Scheduler::instance().create_task(
        []() {}, stack_space, sizeof(stack_space), TaskPriority::Normal, 10, TaskPrivilege::User);

    ASSERT_NE(task, nullptr);

    // 为该任务配置 VAS
    EXPECT_TRUE(mmu.create_task_vas(task));
    EXPECT_TRUE(mmu.map_user_memory(task, USER_SPACE_BASE, 0x80000000, MMU_PAGE_SIZE, MapFlags::Read));
    EXPECT_LT(mmu.get_free_pages(), initial_free);

    // 调用 Scheduler::free_task 释放任务
    Scheduler::instance().free_task(task);
    EXPECT_EQ(task->memory.vasp, nullptr);
    EXPECT_EQ(task->memory.pgdir_base, 0u);

    // 验证所有页表已全部释放
    EXPECT_EQ(mmu.get_free_pages(), initial_free);
}
