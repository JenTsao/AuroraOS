#include "config/autoconf.h"

.syntax unified
.cpu cortex-m3
.thumb

.global _start
.global Reset_Handler

// =====================================================================
// ARMv7-M 向量表 (16 项系统异常)
// =====================================================================
.section .isr_vector, "a", %progbits
    .type g_pfnVectors, %object
g_pfnVectors:
    .word _estack
    .word Reset_Handler
    .word NMI_Handler
    .word HardFault_Handler
    .word MemManage_Handler
    .word BusFault_Handler
    .word UsageFault_Handler
    .word 0
    .word 0
    .word 0
    .word 0
    .word SVC_Handler
    .word DebugMon_Handler
    .word 0
    .word PendSV_Handler
    .word SysTick_Handler
    .size g_pfnVectors, . - g_pfnVectors

// =====================================================================
// Reset_Handler: 上电复位入口
// =====================================================================
    .section .text
    .thumb_func
    .weak Reset_Handler
    .type Reset_Handler, %function
Reset_Handler:
    ldr r0, =_estack
    mov sp, r0

    ldr r0, =_sidata
    ldr r1, =_sdata
    ldr r2, =_edata
copy_data:
    cmp r1, r2
    bge zero_bss
    ldr r3, [r0]
    str r3, [r1]
    adds r0, r0, #4
    adds r1, r1, #4
    b copy_data

zero_bss:
    ldr r0, =_sbss
    ldr r1, =_ebss
    movs r2, #0
zero_loop:
    cmp r0, r1
    bge kernel_entry
    str r2, [r0]
    adds r0, r0, #4
    b zero_loop

kernel_entry:
    bl kernel_main
    b .

// =====================================================================
// 默认异常处理 (weak, 可被 boot/interrupts.cpp 中的强符号覆盖)
// =====================================================================
    .thumb_func
    .weak Default_Handler
    .type Default_Handler, %function
Default_Handler:
    b .

    .weak NMI_Handler
    .thumb_set NMI_Handler, Default_Handler

    .weak HardFault_Handler
    .thumb_set HardFault_Handler, Default_Handler

    .weak MemManage_Handler
    .thumb_set MemManage_Handler, Default_Handler

    .weak BusFault_Handler
    .thumb_set BusFault_Handler, Default_Handler

    .weak UsageFault_Handler
    .thumb_set UsageFault_Handler, Default_Handler

    .weak DebugMon_Handler
    .thumb_set DebugMon_Handler, Default_Handler

// =====================================================================
// PendSV_Handler: 抢占式上下文切换引擎
//
// 硬件自动压栈: xPSR, PC, LR, R12, R3-R0 (8 words → PSP)
// 软件手动保存: R4-R11 (8 words → PSP)
// 总上下文: 16 words = 64 bytes (无 FPU 扩展帧)
// =====================================================================
.global PendSV_Handler
.thumb_func
.type PendSV_Handler, %function
PendSV_Handler:
    cpsid i

    // ── 保存当前任务上下文 ──
    mrs r0, psp
    stmdb r0!, {r4-r11}

    ldr r1, =g_current_tcb_ptr
    ldr r1, [r1]
    str r0, [r1]

    // ── 切换 TCB 指针 ──
    ldr r2, =g_next_tcb_ptr
    ldr r2, [r2]

    ldr r3, =g_current_tcb_ptr
    str r2, [r3]

    // ── 动态特权级切换 (CONTROL.nPRIV) ──
    ldr r3, [r2, #4]
    mrs r0, control
    bfi r0, r3, #0, #1
    msr control, r0
    isb

    // ── MPU 沙盒切换 ──
    push {r2, lr}
    mov r0, r2
    bl mpu_switch_sandbox
    pop {r2, lr}

    // ── 恢复新任务上下文 ──
    ldr r0, [r2]
    ldmia r0!, {r4-r11}
    msr psp, r0

    // ── 性能度量钩子 ──
    push {lr}
    bl pendsv_metrics_hook
    pop {lr}

    cpsie i
    bx lr

// =====================================================================
// SVC_Handler: 系统调用入口
// 通过 EXC_RETURN bit2 判断异常前使用 MSP 还是 PSP
// =====================================================================
.global SVC_Handler
.thumb_func
.type SVC_Handler, %function
SVC_Handler:
    tst lr, #4
    ite eq
    mrseq r0, msp
    mrsne r0, psp
    b SVC_Handler_C
