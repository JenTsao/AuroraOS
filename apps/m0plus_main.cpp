/*
 * auroraOS Cortex-M0+ 极简适配入口 — 适配 STM32L031K6 (8KB SRAM)
 *
 * 启用功能：调度器 + Shell + UART + 可选 ProcFS
 * 为节省 RAM 主动剪裁：
 *   - 不含 metrics (每个 LatencyRecorder 带 100 项 history 数组 ≈ 400B，METRIC_MAX=4 共 ~1.7KB)
 *   - shell_stack 从 512 降为 192 words
 *   - idle_stack 从 128 降为 64 words
 *   - 堆空间 = [__bss_end .. _estack - 0x400]，剩余均给动态分配
 */
#include "shell.hpp"
#include "memory.hpp"
#include "vfs.hpp"
#include "procfs.hpp"
#include "device.hpp"
#include "syscall.hpp"
#include "task.hpp"
#include "uart_device.hpp"

#ifndef CONFIG_BOARD_NUCLEO_L031K6
#include "metrics.hpp"
#endif

extern "C" uint32_t _heap_start;
extern "C" uint32_t _heap_end;
extern "C" void uart_init(void);

// Shell 入口
void shell_task_entry(void) {
    Shell::run();
}

// 极简空闲任务
void idle_task_entry(void) {
    while (true) {
        Arch::wait_for_interrupt();
    }
}

extern "C" void kernel_main(void) {
    uart_init();
    KernelHeap::instance().init(&_heap_start, &_heap_end);

    // 初始化 VFS
    VfsManager::instance().init();

    // 挂载 UART 设备
    static UartDevice uart0_dev("uart0");
    DeviceRegistry::instance().register_device(&uart0_dev);

#ifdef CONFIG_FS_PROCFS
    // 挂载 ProcFS 诊断节点 (仅两个核心节点，节省 RAM)
    static MemInfoNode meminfo_node;
    VfsManager::instance().mount("/proc/meminfo", &meminfo_node);
    static TaskInfoNode taskinfo_node;
    VfsManager::instance().mount("/proc/taskinfo", &taskinfo_node);
#endif

#ifndef CONFIG_BOARD_NUCLEO_L031K6
    Metrics::init();
#endif

    // 初始化调度器
    Scheduler& sched = Scheduler::instance();
    sched.init();

    // 创建空闲任务 — 仅 64 words 栈
    constexpr uint32_t STACK_SIZE_IDLE = 64;
    static uint32_t idle_stack[STACK_SIZE_IDLE];
    sched.create_task(idle_task_entry, idle_stack, STACK_SIZE_IDLE * sizeof(uint32_t),
        TaskPriority::Idle);

    // 创建 Shell 任务 — M0+ 用 192 words 栈（392 字节 shell 代码 + 调度上下文够用）
    constexpr uint32_t STACK_SIZE_SHELL = 192;
    static uint32_t shell_stack[STACK_SIZE_SHELL];
    sched.create_task(shell_task_entry, shell_stack, STACK_SIZE_SHELL * sizeof(uint32_t),
        TaskPriority::High);

    // 启动调度器
    sched.start();
}
