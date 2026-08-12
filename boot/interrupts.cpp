#include "interrupts.hpp"
#include "uart.h"
#include "task.hpp"
#include "syscall.hpp"
#include "../kernel/core/syscall_dispatcher.hpp"
#include "timer.hpp"
#include "work_queue.hpp"
#include "mpu.hpp"
#include "../kernel/core/cspace.hpp"
#include "../kernel/core/ipc.hpp"
#include "../kernel/core/audit.hpp"
using auroraos::kernel::CSpace;
#include "frame_scheduler_v2.hpp"
#include "../metrics/metrics.hpp"

volatile uint32_t isr_enter_cycle = 0;

// =====================================================================
// 软件周期计数器 — 供 Cortex-M0+ 使用（无 DWT CYCCNT）
// 在 SysTick_Handler 中每次 tick 递增，精度为 1 tick（1ms）
// =====================================================================
volatile uint32_t g_sw_cycle_count = 0;

// ────────────────────────────────────────────────────────────────
// SyscallValidator — kernel-side parameter validation for SVC calls.
// All functions are noexcept; they never throw or call user code.
// ────────────────────────────────────────────────────────────────
namespace SyscallValidator {

// Flash region boundaries exposed by the linker script.
// Defined as weak to allow host-test stubs to override them.
extern "C" __attribute__((weak)) uint32_t _flash_start;
extern "C" __attribute__((weak)) uint32_t _flash_end;

// Validate that [ptr, ptr+len) lies entirely within either:
//   (a) the calling task’s stack region, or
//   (b) the read-only Flash segment (for string literals).
// Returns true if the range is safe to dereference in kernel context.
[[nodiscard]] inline bool validate_user_ptr(
        const void* ptr, size_t len,
        uintptr_t task_stack_base, size_t task_stack_size) noexcept
{
    if (!ptr) return false;
    const uintptr_t p   = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t end = p + len;
    // Integer wrap-around check
    if (end < p) return false;

    // (a) Within the task’s own stack
    if (task_stack_size > 0) {
        const bool in_stack = (p >= task_stack_base) &&
                              (end <= task_stack_base + task_stack_size);
        if (in_stack) return true;
    }

    // (b) Within read-only Flash (for string literals passed as SYS_PRINT arg)
    const uintptr_t flash_s = reinterpret_cast<uintptr_t>(&_flash_start);
    const uintptr_t flash_e = reinterpret_cast<uintptr_t>(&_flash_end);
    if (flash_s != flash_e) {  // linker symbols valid
        const bool in_flash = (p >= flash_s) && (end <= flash_e);
        if (in_flash) return true;
    }

    return false;
}

}  // namespace SyscallValidator


// 供 PendSV 汇编读取的两个全局 TCB 指针
// 声明为非 volatile：汇编直接使用符号地址，编译器临界区内通过 Arch:: 保护
extern "C" {
    TaskControlBlock* volatile g_current_tcb_ptr = nullptr;
    TaskControlBlock* volatile g_next_tcb_ptr    = nullptr;
    volatile uint32_t g_switch_start_cycle = 0;

    // 由 PendSV_Handler 调用的 MPU 动态沙盒切换
    // RISC-V 不使用 ARM MPU 路径: PMP 入口由 trap.cpp 在任务切换时统一管理
    void mpu_switch_sandbox(TaskControlBlock* next) {
#if !defined(ARCH_RISCV32)
        if (next && next->memory.size_pow2 > 0) {
            MPU::instance().update_user_sandbox_verified(next->memory.mpu_sandbox);
        }
#else
        (void)next; // RISC-V: PMP 区域在 trap_handler_c 的上下文切换路径中更新
#endif
    }

    void pendsv_metrics_hook() {
        if (g_switch_start_cycle > 0) {
            Metrics::record(METRIC_CTX_SWITCH, Arch::get_cycle() - g_switch_start_cycle);
            g_switch_start_cycle = 0;
        }
    }
}

// 系统 Tick 计数器（全局可见，供 lwIP OSAL 等读取系统时间）
volatile uint32_t tick_count = 0;

extern "C" {
    // ================================================================
    // SVC 分发处理函数（由 boot.S 中的 SVC_Handler 调用）
    // frame 是硬件自动压栈的寄存器快照，通过它读取系统调用参数
    // ================================================================
    void SVC_Handler_C(InterruptFrame* frame) {
        isr_enter_cycle = Arch::get_cycle();
#if defined(ARCH_RISCV32)
        const uint8_t svc_number = static_cast<uint8_t>(frame->svc_num);
#else
        // 通过 PC 回溯到 SVC 指令，提取 8 位系统调用号
        const uint16_t svc_instr = reinterpret_cast<const uint16_t*>(frame->pc)[-1];
        const uint8_t  svc_number = static_cast<uint8_t>(svc_instr & 0xFF);
#endif

        // 获取当前任务的栈边界，用于参数指针校验
        auroraos::kernel::SyscallDispatcher::dispatch(frame, svc_number);
    }

    // ================================================================
    // 内存管理异常处理（捕捉 MPU 违规访问）
    // ================================================================
    static void aurora_dbg_print_hex(uint32_t v) {
        uart_puts("0x");
        for (int shift = 28; shift >= 0; shift -= 4) {
            uint32_t nibble = (v >> shift) & 0xF;
            uart_putc(nibble < 10 ? ('0' + nibble) : ('A' + nibble - 10));
        }
    }

#if !defined(BOARD_MCU_STM32L031K6)
    // ARMv7-M: MemManage 有独立异常号
    void MemManage_Handler(void) {
        volatile uint32_t* cfsr  = reinterpret_cast<volatile uint32_t*>(0xE000ED28U);
        volatile uint32_t* mmfar = reinterpret_cast<volatile uint32_t*>(0xE000ED34U);
        uint32_t cfsr_val  = *cfsr;
        uint32_t mmfar_val = *mmfar;
        uart_puts("\r\n[MemManage_Handler] Memory Protection Violation Detected! \r\n");
        uart_puts("CFSR = ");  aurora_dbg_print_hex(cfsr_val);  uart_puts("\r\n");
        uart_puts("MMFAR= ");  aurora_dbg_print_hex(mmfar_val); uart_puts("\r\n");
        uart_puts("Access Denied! Offending thread terminated by kernel.\r\n");

        TaskControlBlock* current = Scheduler::instance().get_current_tcb();
        if (current) {
            current->scheduler.state = TaskState::Terminated;
        }

        Scheduler::instance().schedule();
        return;
    }

    void HardFault_Handler(void) {
        uart_puts("\r\n[HardFault_Handler] Hard Fault Detected! System Halted.\r\n");
        while (1) {}
    }

    // BusFault / UsageFault were previously bound to the silent Default_Handler
    // (infinite loop) in boot.S, so any fault there produced a silent hang with
    // no output. Wire them to diagnostic printers so HIL/CI failures are visible.
    void BusFault_Handler(void) {
        volatile uint32_t* cfsr = reinterpret_cast<volatile uint32_t*>(0xE000ED28U);
        volatile uint32_t* bfar = reinterpret_cast<volatile uint32_t*>(0xE000ED38U);
        uint32_t cfsr_val = *cfsr;
        uint32_t bfar_val = *bfar;
        uart_puts("\r\n[BusFault_Handler] Bus Fault Detected! \r\n");
        uart_puts("CFSR = "); aurora_dbg_print_hex(cfsr_val); uart_puts("\r\n");
        uart_puts("BFAR = "); aurora_dbg_print_hex(bfar_val); uart_puts("\r\n");
        while (1) {}
    }

    void UsageFault_Handler(void) {
        volatile uint32_t* cfsr = reinterpret_cast<volatile uint32_t*>(0xE000ED28U);
        uint32_t cfsr_val = *cfsr;
        uart_puts("\r\n[UsageFault_Handler] Usage Fault Detected! \r\n");
        uart_puts("CFSR = "); aurora_dbg_print_hex(cfsr_val); uart_puts("\r\n");
        while (1) {}
    }

    void NMI_Handler(void) {
        uart_puts("\r\n[NMI_Handler] Non-Maskable Interrupt! System Halted.\r\n");
        while (1) {}
    }
#else
    // ARMv6-M (Cortex-M0+): MemManage/BusFault/UsageFault 全部合并到 HardFault
    // 由 boot.S 中的 HardFault_Handler 汇编入口调用此 C 函数
    extern "C" void MemManage_Handler_C(uint32_t cfsr_val) {
        uart_puts("\r\n[HardFault] Fault on Cortex-M0+ (all faults collapse here) \r\n");
        uart_puts("CFSR = ");  aurora_dbg_print_hex(cfsr_val);  uart_puts("\r\n");
        uart_puts("Access Denied! Offending thread terminated by kernel.\r\n");

        TaskControlBlock* current = Scheduler::instance().get_current_tcb();
        if (current) {
            current->scheduler.state = TaskState::Terminated;
        }

        Scheduler::instance().schedule();
    }
#endif
}

// ================================================================
// SysTick 中断：系统心跳，驱动两件事：
//   1. tick_update()  — 将到期的休眠任务唤醒（设为 Ready）
//   2. schedule()     — 每 10ms 触发一次调度：
//                       * 高优先级任务唤醒后立即抢占低优先级
//                       * 同级任务轮转时间片
// ================================================================

#ifdef CONFIG_WATCHDOG
#include "../kernel/core/watchdog_manager.hpp"
#endif


void SysTick_Handler(void) {
    isr_enter_cycle = Arch::get_cycle();
    g_sw_cycle_count++;    // 软件周期计数器递增（M0+ 无 DWT 时用此替代）
    tick_count++;
    
    // 1. 驱动软件定时器引擎
    TimerManager::instance().on_tick();

    // 2. 【核心注入】驱动蓝河帧感知时钟窗 (计算 33ms 边界)
    FrameSchedulerV2::instance().on_tick(1);  // 1 tick = 1ms at 1000Hz

    Scheduler& sched = Scheduler::instance();
    sched.tick_update();

#ifdef CONFIG_WATCHDOG
    // 3. 软件看门狗 tick 驱动（硬件 WDT 由硬件独立计时，此处无操作）
    //    软件 WDT（QEMU 等无物理看门狗平台）在此递减计数器
    {
        WatchdogDriver* wdt = WatchdogManager::instance().get_driver();
        if (wdt && wdt->on_tick()) {
            uart_puts("\r\n[FATAL] Software watchdog timeout! System halted.\r\n");
            WatchdogManager::instance().disable();
            while (true) {}
        }
    }
#endif
    
    // 移除 5ms 强制约束，改为每个 SysTick 周期 (1ms) 都触发时间片重新评估
    // 保障蓝河帧感知时钟窗等硬实时机制在边界处 0ms 延迟立即抢占
    sched.schedule(); 
}
