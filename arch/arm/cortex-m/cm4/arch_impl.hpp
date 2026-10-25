#ifndef ARCH_IMPL_HPP
#define ARCH_IMPL_HPP

#include "../common/arch_cm_common.hpp"

// =====================================================================
// ARM Cortex-M4 架构实现（ARMv7E-M，无 FPU 变体走本文件；
// 带 FPU 的 Apollo3 M4F 目标使用 cm4f 目录）
//
// 与 cm3 的唯一功能差异：wait_for_interrupt 使用裸 wfi
// （不带 dsb/isb 序列），空闲进入路径更短。
// 公共部分（中断/BASEPRI/DWT/SysTick/Tickless/MPU/特权级/切换触发）
// 见 ../common/arch_cm_common.hpp。
// =====================================================================

namespace Arch {

// =====================================================================
// 低功耗等待（M4 变体：裸 wfi，无 dsb/isb 序列）
// =====================================================================
inline void wait_for_interrupt() {
    __asm__ volatile("wfi" : : : "memory");
}

// =====================================================================
// 线程初始栈帧伪造
//
// 栈帧布局 (高地址 → 低地址):
//   xPSR | PC | LR(EXC_RETURN) | R12 | R3 | R2 | R1 | R0 | R11..R4
//   [--- 硬件异常帧 (8 words) ---]  [--- 软件保存 (8 words) ---]
// =====================================================================
inline uint32_t* init_thread_stack(void (*task_entry)(void), uint32_t* stack_space, uint32_t stack_size) {
    uint32_t* top = stack_space + (stack_size / sizeof(uint32_t));

    top--;
    *top = XPSR_THUMB; // xPSR
    top--;
    *top = reinterpret_cast<uint32_t>(task_entry); // PC
    top--;
    *top = EXC_RETURN_PSP; // LR
    top -= 5;              // R12, R3, R2, R1, R0
    top -= 8;              // R11 ~ R4

    return top;
}

// =====================================================================
// 引导第一个任务
// =====================================================================
[[noreturn]] inline void start_first_task(uint32_t* stack_ptr, void (*entry_point)(), uint32_t privilege = 0) {
    __asm__ volatile("ldm  %0!, {r4-r11}  \n\t"
                     "msr  psp, %0        \n\t"
                     "mov  r0, #2         \n\t"
                     "orr  r0, r0, %2     \n\t"
                     "msr  control, r0   \n\t"
                     "isb                 \n\t"
                     "cpsie i             \n\t"
                     "bx   %1             \n\t"
                     :
                     : "r"(stack_ptr), "r"(reinterpret_cast<uint32_t>(entry_point)), "r"(privilege)
                     : "r0", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "memory");
    __builtin_unreachable();
}

} // namespace Arch

#endif // ARCH_IMPL_HPP
