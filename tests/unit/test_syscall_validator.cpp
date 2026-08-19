// =============================================================================
// tests/unit/test_syscall_validator.cpp
//
// SyscallValidator 全面校验测试
// 覆盖：栈、MPU 沙盒、堆缓冲区、全局数据/BSS段、MMU 用户页、只读 Flash
// =============================================================================
#include <gtest/gtest.h>
#include <cstring>

#include "../../kernel/core/syscall_validator.hpp"
#include "../../kernel/task/task.hpp"
#include "../../kernel/mm/memory.hpp"
#include "../../kernel/mm/mmu.hpp"

using namespace auroraos::kernel;

// 模拟链接脚本段边界
static uint8_t mock_flash[1024];
static uint8_t mock_sdata[512];
static uint8_t mock_sbss[512];

extern "C" {
    extern uintptr_t _flash_start;
    extern uintptr_t _flash_end;
    extern uintptr_t _sdata;
    extern uintptr_t _edata;
    extern uintptr_t _sbss;
    extern uintptr_t _ebss;
}

class SyscallValidatorTest : public ::testing::Test {
protected:
    static uint8_t heap_pool[64 * 1024];
    static uint8_t page_pool[2 * 1024 * 1024];

    void SetUp() override {
        _flash_start = reinterpret_cast<uintptr_t>(mock_flash);
        _flash_end   = reinterpret_cast<uintptr_t>(mock_flash + sizeof(mock_flash));
        _sdata       = reinterpret_cast<uintptr_t>(mock_sdata);
        _edata       = reinterpret_cast<uintptr_t>(mock_sdata + sizeof(mock_sdata));
        _sbss        = reinterpret_cast<uintptr_t>(mock_sbss);
        _ebss        = reinterpret_cast<uintptr_t>(mock_sbss + sizeof(mock_sbss));

        KernelHeap::instance().init(heap_pool, heap_pool + sizeof(heap_pool));
        Mmu::instance().init(page_pool, sizeof(page_pool));
    }
};

uint8_t SyscallValidatorTest::heap_pool[64 * 1024];
uint8_t SyscallValidatorTest::page_pool[2 * 1024 * 1024];

// =============================================================================
// 1. 基础异常防护（NULL、溢出越界、0 长度）
// =============================================================================
TEST_F(SyscallValidatorTest, BasicBoundaryAndOverflowProtection) {
    TaskControlBlock task{};
    task.memory.stack_base = 0x20001000;
    task.memory.size_pow2 = 10; // 1024 bytes (0x20001000 ~ 0x20001400)

    // NULL 指针必须拒绝
    EXPECT_FALSE(SyscallValidator::validate_user_ptr(nullptr, 16, &task, false));
    EXPECT_FALSE(SyscallValidator::validate_user_ptr(nullptr, 0, &task, false));

    // 0 长度且非空指针视为安全
    uint8_t dummy = 0;
    EXPECT_TRUE(SyscallValidator::validate_user_ptr(&dummy, 0, &task, false));

    // 整数溢出 / 回绕指针必须拒绝
    const void* overflow_ptr = reinterpret_cast<const void*>(UINTPTR_MAX - 8);
    EXPECT_FALSE(SyscallValidator::validate_user_ptr(overflow_ptr, 16, &task, false));
}

// =============================================================================
// 2. 任务私有栈空间校验
// =============================================================================
TEST_F(SyscallValidatorTest, TaskStackValidation) {
    uint8_t stack_mem[1024];
    TaskControlBlock task{};
    task.memory.stack_base = reinterpret_cast<uintptr_t>(stack_mem);
    task.memory.size_pow2 = 10; // 1024 bytes

    // 内部合法区间
    EXPECT_TRUE(SyscallValidator::validate_user_ptr(stack_mem, 100, &task, false));
    EXPECT_TRUE(SyscallValidator::validate_user_ptr(stack_mem + 500, 200, &task, true));
    EXPECT_TRUE(SyscallValidator::validate_user_ptr(stack_mem + 1023, 1, &task, true));

    // 跨越栈边界
    EXPECT_FALSE(SyscallValidator::validate_user_ptr(stack_mem + 1000, 50, &task, false));
    EXPECT_FALSE(SyscallValidator::validate_user_ptr(stack_mem - 10, 20, &task, false));
}

// =============================================================================
// 3. 用户堆 (Heap) 动态分配缓冲区校验
// =============================================================================
TEST_F(SyscallValidatorTest, HeapBufferValidation) {
    TaskControlBlock task{};
    task.memory.stack_base = 0x20001000;
    task.memory.size_pow2 = 10;

    void* heap_buf = KernelHeap::instance().allocate(256);
    ASSERT_NE(heap_buf, nullptr);

    // 堆缓冲区必须同时被读、写系统调用接受
    EXPECT_TRUE(SyscallValidator::validate_user_ptr(heap_buf, 256, &task, false));
    EXPECT_TRUE(SyscallValidator::validate_user_ptr(heap_buf, 256, &task, true));
    EXPECT_TRUE(SyscallValidator::validate_user_ptr(heap_buf, 128, &task, true));

    // 堆越界检查
    uintptr_t heap_end = KernelHeap::instance().get_heap_end();
    EXPECT_FALSE(SyscallValidator::validate_user_ptr(reinterpret_cast<void*>(heap_end - 10), 50, &task, false));

    KernelHeap::instance().deallocate(heap_buf);
}

// =============================================================================
// 4. 全局数据段 (Data) 与 BSS 段校验
// =============================================================================
TEST_F(SyscallValidatorTest, DataAndBssValidation) {
    TaskControlBlock task{};
    task.memory.stack_base = 0x20001000;
    task.memory.size_pow2 = 10;

    // 全局静态数据段 (Data)
    EXPECT_TRUE(SyscallValidator::validate_user_ptr(mock_sdata, sizeof(mock_sdata), &task, false));
    EXPECT_TRUE(SyscallValidator::validate_user_ptr(mock_sdata, sizeof(mock_sdata), &task, true));

    // 全局零初始化段 (BSS)
    EXPECT_TRUE(SyscallValidator::validate_user_ptr(mock_sbss, sizeof(mock_sbss), &task, false));
    EXPECT_TRUE(SyscallValidator::validate_user_ptr(mock_sbss, sizeof(mock_sbss), &task, true));

    // 越界 Data/BSS
    EXPECT_FALSE(SyscallValidator::validate_user_ptr(mock_sdata + sizeof(mock_sdata) - 4, 16, &task, false));
}

// =============================================================================
// 5. 只读 Flash / 常量字符串校验 (Read-Only vs Write Protection)
// =============================================================================
TEST_F(SyscallValidatorTest, FlashReadOnlyProtection) {
    TaskControlBlock task{};
    task.memory.stack_base = 0x20001000;
    task.memory.size_pow2 = 10;

    // Flash 只读访问 (例如 SYS_PRINT 传入字符串常量) -> 允许
    EXPECT_TRUE(SyscallValidator::validate_user_ptr(mock_flash, sizeof(mock_flash), &task, false));
    EXPECT_TRUE(SyscallValidator::validate_user_ptr(mock_flash + 10, 50, &task, false));

    // Flash 写操作 (例如 read 系统调用以 Flash 为目标缓冲区) -> 严格拒绝！
    EXPECT_FALSE(SyscallValidator::validate_user_ptr(mock_flash, sizeof(mock_flash), &task, true));
    EXPECT_FALSE(SyscallValidator::validate_user_ptr(mock_flash + 10, 50, &task, true));
}

// =============================================================================
// 6. MMU 虚拟地址空间已映射页校验 (User MMU Pages)
// =============================================================================
TEST_F(SyscallValidatorTest, MmuUserPageValidation) {
    TaskControlBlock task{};
    task.scheduler.id = 10;

    Mmu& mmu = Mmu::instance();
    EXPECT_TRUE(mmu.create_task_vas(&task));

    uintptr_t user_rw_va = USER_SPACE_BASE;
    uintptr_t user_ro_va = USER_SPACE_BASE + 0x10000;
    uintptr_t kernel_only_va = 0x40000000; // 特权内核 RAM

    // 映射用户读写页
    EXPECT_TRUE(mmu.map_user_memory(&task, user_rw_va, 0x60000000, MMU_PAGE_SIZE,
                                    MapFlags::Read | MapFlags::Write));

    // 映射用户只读页
    EXPECT_TRUE(mmu.map_user_memory(&task, user_ro_va, 0x60001000, MMU_PAGE_SIZE,
                                    MapFlags::Read));

    // 1. 用户读写页：读写均合法
    EXPECT_TRUE(SyscallValidator::validate_user_ptr(reinterpret_cast<void*>(user_rw_va), 100, &task, false));
    EXPECT_TRUE(SyscallValidator::validate_user_ptr(reinterpret_cast<void*>(user_rw_va), 100, &task, true));

    // 2. 用户只读页：只读合法，写操作被拒绝
    EXPECT_TRUE(SyscallValidator::validate_user_ptr(reinterpret_cast<void*>(user_ro_va), 100, &task, false));
    EXPECT_FALSE(SyscallValidator::validate_user_ptr(reinterpret_cast<void*>(user_ro_va), 100, &task, true));

    // 3. 内核特权页 (未设 User 位)：被严格拒绝
    EXPECT_FALSE(SyscallValidator::validate_user_ptr(reinterpret_cast<void*>(kernel_only_va), 100, &task, false));
    EXPECT_FALSE(SyscallValidator::validate_user_ptr(reinterpret_cast<void*>(kernel_only_va), 100, &task, true));

    // 4. 未映射的虚拟地址：被拒绝
    EXPECT_FALSE(SyscallValidator::validate_user_ptr(reinterpret_cast<void*>(0x12340000), 100, &task, false));

    mmu.destroy_task_vas(&task);
}
