#ifndef ARCH_IMPL_HPP
#define ARCH_IMPL_HPP

#include "../common/arch_cm_common.hpp"

// =====================================================================
// ARMv7E-M (Cortex-M7) 架构实现
//
// 指令集特性: 完整 Thumb-2 + DSP/SIMD (ARMv7E-M: smlad/smuad 等)
// FPU: FPv5-SP (单精度, 32 个 S 寄存器), 在 early_init.cpp 中使能,
//      上下文切换由 cm7/context_switch.S 惰性保存 S16-S31
// Cache: 32-byte line 的 I/D-Cache, 在 early_init.cpp 冷启动使能
// 硬件周期计数: DWT CYCCNT
// 内存保护: PMSAv7, 最多 16 region (本实现沿用 8-region API, 索引兼容)
//
// 公共部分（中断/BASEPRI/DWT/SysTick/Tickless/MPU/特权级/切换触发）
// 见 ../common/arch_cm_common.hpp，本文件仅保留 M7 特有差异：
// FPU 感知的 8+9 词哨兵值栈帧与配套的启动序列。
// =====================================================================

namespace Arch {

// =====================================================================
// 低功耗等待：dsb 保证此前存储完成后再 WFI，isb 保证唤醒后指令流重取
// =====================================================================
inline void wait_for_interrupt() {
    __asm__ volatile("dsb\n\t"
                     "wfi\n\t"
                     "isb\n\t"
                     :
                     :
                     : "memory");
}

// =====================================================================
// 线程初始栈帧伪造
//
// FPU 感知布局 (高地址 → 低地址):
//   xPSR | PC | LR(EXC_RETURN) | R12 | R3 | R2 | R1 | R0 | EXC_RETURN | R11..R4
//   [------ 硬件异常帧 (8 words) ------]  [--- 软件保存 (9 words) ---]
// 软件帧首字为 EXC_RETURN = 0xFFFFFFFD (无 FPU 扩展帧), 与
// cm7/context_switch.S 的 ldmia r0!, {r4-r11, lr} 恢复序列配套。
// 各寄存器预置哨兵值，便于调试器识别未初始化栈帧。
// =====================================================================
inline uint32_t* init_thread_stack(void (*task_entry)(void), uint32_t* stack_space, uint32_t stack_size) {
    uint32_t* top = stack_space + (stack_size / sizeof(uint32_t));

    // 1. 硬件异常帧 (8 words, 8-byte aligned)
    top--;
    *top = XPSR_THUMB; // xPSR: Thumb 位
    top--;
    *top = reinterpret_cast<uint32_t>(task_entry); // PC: 任务入口
    top--;
    *top = EXC_RETURN_PSP; // LR: Thread mode + PSP, 无 FPU 扩展帧
    top--;
    *top = 0x12121212; // R12
    top--;
    *top = 0x03030303; // R3
    top--;
    *top = 0x02020202; // R2
    top--;
    *top = 0x01010101; // R1
    top--;
    *top = 0x00000000; // R0: 任务参数占位

    // 2. 软件保存帧 (9 words) — 对应 context_switch.S 的 stmdb/ldmia {r4-r11, lr}
    top--;
    *top = EXC_RETURN_PSP; // EXC_RETURN (PendSV 首次恢复时读取, 识别 FPU 惰性状态)
    top--;
    *top = 0x11111111; // R11
    top--;
    *top = 0x10101010; // R10
    top--;
    *top = 0x09090909; // R9
    top--;
    *top = 0x08080808; // R8
    top--;
    *top = 0x07070707; // R7
    top--;
    *top = 0x06060606; // R6
    top--;
    *top = 0x05050505; // R5
    top--;
    *top = 0x04040404; // R4

    return top;
}

// =====================================================================
// 引导第一个任务（恢复序列含 lr，与 9 词软件帧配套）
// =====================================================================
[[noreturn]] inline void start_first_task(uint32_t* stack_ptr, void (*entry_point)(), uint32_t privilege = 0) {
    __asm__ volatile("ldm  %0!, {r4-r11, lr} \n\t" // 弹出 R4-R11 和 EXC_RETURN
                     "msr  psp, %0           \n\t"
                     "mov  r0, #2            \n\t" // CONTROL = 0b10: Thread mode + PSP (FPCA=0, FPU 惰性未激活)
                     "orr  r0, r0, %2        \n\t" // 合并特权级 (0: Kernel->2, 1: User->3)
                     "msr  control, r0      \n\t"
                     "isb                    \n\t"
                     "cpsie i                \n\t"
                     "bx   %1                \n\t"
                     :
                     : "r"(stack_ptr), "r"(reinterpret_cast<uint32_t>(entry_point)), "r"(privilege)
                     : "r0", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "lr", "memory");
    __builtin_unreachable();
}

} // namespace Arch

#endif // ARCH_IMPL_HPP
