#ifndef INTERRUPTS_HPP
#define INTERRUPTS_HPP

#include <stdint.h>

struct InterruptFrame {
#if defined(ARCH_AARCH64)
    // 64-bit general purpose registers
    uint64_t x[30];    // x0 to x29
    uint64_t lr;       // x30 (Link Register)
    uint64_t elr;      // Exception Link Register
    uint64_t spsr;     // Saved Program Status Register
    uint64_t sp_el0;   // User stack pointer
#else
    // Shared argument registers for syscalls
    union { uint32_t r0; uint32_t arg0; };
    union { uint32_t r1; uint32_t arg1; };
    union { uint32_t r2; uint32_t arg2; };
    union { uint32_t r3; uint32_t arg3; };

#if !defined(ARCH_RISCV32)
    // ARM specific hardware-stacked registers
    uint32_t r12;
    uint32_t lr;
#endif

    uint32_t pc;

#if !defined(ARCH_RISCV32)
    uint32_t psr;
#endif

#if defined(ARCH_RISCV32)
    uint32_t svc_num; // For RISC-V to pass a7
#endif

#endif // ARCH_AARCH64
};



extern "C" {
    void PendSV_Handler();
    void SysTick_Handler();
    void SVC_Handler_C(InterruptFrame* frame); // 由汇编或 trap.cpp 调用
}

#endif
