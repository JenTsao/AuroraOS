/*
 * auroraOS Cortex-M0+ 极简入口
 * 仅包含调度器 + Shell + UART，验证 M0+ 架构移植的正确性。
 * 不包含：Lua、LittleFS、UI 渲染、网络、传感器、BLE 等。
 */
#include "shell.hpp"
#include "memory.hpp"
#include "vfs.hpp"
#include "device.hpp"
#include "ramfs.hpp"
#include "syscall.hpp"
#include "task.hpp"
#include "uart_device.hpp"

extern "C" uint32_t _heap_start;
extern "C" uint32_t _heap_end;
extern "C" void uart_init(void);

extern "C" void shell_task(void);

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
    sched.create_task(shell_task, shell_stack, STACK_SIZE_SHELL * sizeof(uint32_t),
        TaskPriority::High);

    // 启动调度器
    sched.start();
}
