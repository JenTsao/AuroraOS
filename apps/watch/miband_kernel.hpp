#ifndef AURORA_MIBAND_KERNEL_HPP
#define AURORA_MIBAND_KERNEL_HPP

#include <stdint.h>
// 引入手环特供的各类底层设施
#include "board.h"
#include "arch_api.hpp"
#include "memory.hpp"
#include "task.hpp"
#include "frame_scheduler_v2.hpp"
#include "power_manager.hpp"
#include "sensor_framework.hpp"
#include "watch_app.hpp"

// ========================================================
// 1. UI 渲染主线程 (优先级: CRITICAL)
// ========================================================
void ui_render_task() {
    // 初始化手环所有上层应用逻辑
    WatchApp::instance().init();

    while (true) {
        // 只有当 PowerManager 允许 (非息屏状态)，且位于当前帧渲染窗口内时，
        // 才会执行实际的脏区域像素推送
        WatchApp::instance().on_frame_render();

        // 将控制权交还给动态帧调度器，严格遵守 30fps/15fps/0fps 的时间窗
        FrameSchedulerV2::instance().wait_for_next_frame();
    }
}

// ========================================================
// 2. 传感器与蓝牙后台守护线程 (优先级: HIGH)
// 即使在息屏 (0fps) 的深度睡眠期，该线程依然被允许定时唤醒运行
// ========================================================
void sensor_ble_daemon_task() {
    uint32_t current_tick = 0;
    const uint32_t DAEMON_TICK_MS = 40; // 40ms 唤醒一次 (匹配 25Hz 的传感器采样率)

    while (true) {
        // 1. 触发一次传感器硬件 FIFO 读取，并将数据压入环形缓冲区
        SensorManager::instance().fetch_and_buffer(current_tick);

        // 2. 驱动表盘后台逻辑 (处理数据同步、BLE 推送与 5 级状态机降级判定)
        WatchApp::instance().on_background_tick(DAEMON_TICK_MS);

        current_tick += DAEMON_TICK_MS;

        // 3. 挂起自身，等待下一次 40ms 周期到来 (在此期间 CPU 可进入 WFI)
        Scheduler::instance().sleep_ms(DAEMON_TICK_MS);
    }
}

// ========================================================
// 3. 系统空闲线程 (优先级: LOWEST)
// 没有任何任务需要运行时，CPU 跌落至此，直接切断时钟树供电
// ========================================================
void idle_task() {
    while (true) {
        // 询问 PowerManager 当前是否处于 SLEEP 或 CRITICAL 状态
        // 如果是，则调用 board_enter_wfi() 让硬件进入最深的睡眠模式
        PowerManager::instance().execute_wfi_if_needed();
    }
}

// ========================================================
// 手环版系统总入口 (被汇编 Reset_Handler 最终调用)
// ========================================================
extern "C" void miband_kernel_main(void) {
    // 1. 硬件级板载初始化 (配置 96MHz 时钟树、外设电源、关闭锁相环等)
    board_hardware_init();
    
    // 2. 架构级初始化 (开启 M4F 硬件浮点运算单元 FPU，配置中断优先级)
    Arch::init();

    // 3. 内存系统初始化 (依赖 linker_miband.ld 中定义的物理地址)
    extern uint32_t _heap_start;
    extern uint32_t end;
    KernelHeap::instance().init(&_heap_start, &end);

    // 4. 微内核调度器就绪
    Scheduler& sched = Scheduler::instance();
    sched.init();

    // ========================================================
    // 5. 极限压榨内存的任务创建 (总计仅允许 MAX_TASKS=8)
    // ========================================================
    
    // UI 线程栈 (分配 1024 uint32_t = 4KB，应对复杂的界面状态机)
    static uint32_t ui_stack[1024];
    uint32_t ui_tid = sched.create_task(ui_render_task, ui_stack, 1024 * sizeof(uint32_t), TaskPriority::Realtime)->id;
    
    // 将 UI 线程绑定到动态帧调度器
    FrameSchedulerV2::instance().init(30, ui_tid);

    // 传感器与 BLE 线程栈 (分配 512 uint32_t = 2KB，处理浮点数和网络封包)
    static uint32_t daemon_stack[512];
    sched.create_task(sensor_ble_daemon_task, daemon_stack, 512 * sizeof(uint32_t), TaskPriority::High);

    // Idle 线程栈 (极简 128 uint32_t = 0.5KB)
    static uint32_t idle_stack[128];
    sched.create_task(idle_task, idle_stack, 128 * sizeof(uint32_t), TaskPriority::Idle); // 绝对最低优先级 0

    // ========================================================
    // 6. 激活内核心跳并点火升空！
    // ========================================================
    
    volatile uint32_t* syst_ctrl = reinterpret_cast<volatile uint32_t*>(0xE000E010);
    volatile uint32_t* syst_load = reinterpret_cast<volatile uint32_t*>(0xE000E014);
    volatile uint32_t* syst_val  = reinterpret_cast<volatile uint32_t*>(0xE000E018);
    *syst_load = (SYSTEM_CORE_CLOCK / 1000) - 1;   // 1ms tick period
    *syst_val  = 0;                                  // clear current value
    *syst_ctrl = (1u << 2) | (1u << 1) | (1u << 0);  // CLKSOURCE=core, TICKINT=on, ENABLE=on

    // 触发系统首次上下文切换，跳入最高优先级的 UI 线程！
    Arch::trigger_context_switch(); 

    while (1) {} // 内核永远不该返回到这里
}

#endif // AURORA_MIBAND_KERNEL_HPP
