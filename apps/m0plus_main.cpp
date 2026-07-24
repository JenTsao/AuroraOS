/*
 * auroraOS Cortex-M0+ 完整适配入口
 *
 * 启用功能：调度器 + Shell + UART + VFS + ProcFS + Metrics
 * 不包含：Lua、LittleFS、UI 渲染、网络、传感器、BLE、OTA（RAM 限制）
 */
#include "shell.hpp"
#include "memory.hpp"
#include "vfs.hpp"
#include "procfs.hpp"
#include "device.hpp"
#include "syscall.hpp"
#include "task.hpp"
#include "metrics.hpp"
#include "uart_device.hpp"

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
    // 挂载 ProcFS 诊断节点 (仅关键节点，节省 Flash)
    static MemInfoNode meminfo_node;
    VfsManager::instance().mount("/proc/meminfo", &meminfo_node);
    static TaskInfoNode taskinfo_node;
    VfsManager::instance().mount("/proc/taskinfo", &taskinfo_node);
    // uptime and caps nodes omitted to fit in 64KB Flash
#endif

    // 初始化 Metrics（M0+ 使用软件计数器）
    Metrics::init();

    // 初始化调度器
    Scheduler& sched = Scheduler::instance();
    sched.init();

    // 创建空闲任务
    constexpr uint32_t STACK_SIZE_IDLE = 128;
    static uint32_t idle_stack[STACK_SIZE_IDLE];
    sched.create_task(idle_task_entry, idle_stack, STACK_SIZE_IDLE * sizeof(uint32_t),
        TaskPriority::Idle);

    // 创建 Shell 任务
    constexpr uint32_t STACK_SIZE_SHELL = 512;
    static uint32_t shell_stack[STACK_SIZE_SHELL];
    sched.create_task(shell_task_entry, shell_stack, STACK_SIZE_SHELL * sizeof(uint32_t),
        TaskPriority::High);

    // 启动调度器
    sched.start();
}
