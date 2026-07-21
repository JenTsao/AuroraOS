/*
 * auroraOS Cortex-M0+ (ARMv6-M) 启动汇编
 *
 * 与 Cortex-M4 (ARMv7E-M) 的关键差异：
 *   - 向量表仅 16 个条目（ARMv6-M 固定格式，无可选 Fault 异常）
 *   - PendSV 仅保存/恢复 r4-r7（Thumb 模式只能访问低寄存器）
 *   - SVC_Handler 无 ITE 条件块，改用两次分支
 *   - 无 ISB 指令（用 nop 替代）
 *   - 无 BFI 指令（CONTROL.nPRIV 用 bics/orrs 替代）
 */

#include "config/autoconf.h"

.syntax unified
.cpu cortex-m0plus
.thumb

.global _start
.global Reset_Handler

/* ================================================================
 * ARMv6-M 固定向量表（16 个条目，不可裁剪）
 * ================================================================ */
.section .isr_vector, "a", %progbits
    .type g_pfnVectors, %object
g_pfnVectors:
    .word _estack             /* 0x00: 初始栈顶 */
    .word Reset_Handler       /* 0x04: 复位向量 */
    .word NMI_Handler         /* 0x08: NMI */
    .word HardFault_Handler   /* 0x0C: HardFault（M0+ 所有 fault 都走这里）*/
    .word 0                   /* 0x10: Reserved */
    .word 0                   /* 0x14: Reserved */
    .word 0                   /* 0x18: Reserved */
    .word 0                   /* 0x1C: Reserved */
    .word 0                   /* 0x20: Reserved */
    .word 0                   /* 0x24: Reserved */
    .word 0                   /* 0x28: Reserved */
    .word SVC_Handler         /* 0x2C: SVCall */
    .word 0                   /* 0x30: Reserved */
    .word 0                   /* 0x34: Reserved */
    .word PendSV_Handler      /* 0x38: PendSV */
    .word SysTick_Handler     /* 0x3C: SysTick */
    .size g_pfnVectors, . - g_pfnVectors

/* ================================================================
 * Reset_Handler: 数据段拷贝 + BSS 清零 + 跳入 C++ kernel_main
 * ================================================================ */
    .section .text
    .thumb_func
    .weak Reset_Handler
    .type Reset_Handler, %function
Reset_Handler:
    ldr r0, =_estack
    mov sp, r0

    /* 拷贝 .data 段从 Flash 到 RAM */
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

    .thumb_func
    .weak Default_Handler
Default_Handler:
    b .

    .weak NMI_Handler
    .thumb_set NMI_Handler, Default_Handler

/* ================================================================
 * HardFault_Handler — M0+ 所有异常（MemManage/BusFault/UsageFault）
 * 全部合并到这里，因为 ARMv6-M 只有 HardFault。
 * ================================================================ */
.global HardFault_Handler
.thumb_func
.type HardFault_Handler, %function
HardFault_Handler:
    /* 读取 SCB->CFSR（如果芯片支持的话，M0+ 部分实现有） */
    ldr r0, =0xE000ED28     /* CFSR 地址 */
    ldr r1, [r0]
    /* 简单处理：打印后挂起（生产环境应终止违规任务） */
    push {r0, r1, r3}       /* 保存 r3 作为临时寄存器（ARMv6-M POP 不能直接弹出 LR）*/
    mov r3, lr              /* 保存 lr 到 r3 */
    push {r3}
    bl MemManage_Handler_C  /* 调用 C 函数做详细处理 */
    pop {r3}
    mov lr, r3              /* 恢复 lr */
    pop {r0, r1, r3}
    bx lr

/* ================================================================
 * PendSV_Handler — 上下文切换核心引擎
 *
 * ARMv6-M 限制：
 *   - 仅 Thumb 指令（16位）
 *   - STM/LDM 仅 r0-r7
 *   - 无 bfi，用 bics/orrs 替代
 *   - 无 isb，用 nop 替代
 * ================================================================ */
#ifndef CONFIG_BOARD_NUCLEO_L031K6
/* 当不是独立 boot.S 构建时，定义 PendSV_Handler */
#endif

.global PendSV_Handler
.thumb_func
.type PendSV_Handler, %function
PendSV_Handler:
    cpsid i                     /* 1. 关中断，保证原子性 */

    mrs r0, psp                 /* 获取当前线程栈指针 PSP */

    /* --- 保存旧线程 r4-r7（仅低寄存器）--- */
    subs r0, r0, #16            /* 在栈上预留 16 字节 (4 x 4) */
    str r0, [sp]                /* 临时保存 r0（M0+ 栈指针操作受限）*/
    mov r1, r0
    str r4, [r1, #0]
    str r5, [r1, #4]
    str r6, [r1, #8]
    str r7, [r1, #12]

    /* --- 将更新后的栈指针写入旧线程 TCB --- */
    ldr r1, =g_current_tcb_ptr
    ldr r1, [r1]                /* r1 = *g_current_tcb_ptr */
    str r0, [r1]                /* tcb->stack_ptr = r0 */

    /* --- 切换 TCB 指针 --- */
    ldr r2, =g_next_tcb_ptr
    ldr r2, [r2]                /* r2 = *g_next_tcb_ptr */

    ldr r3, =g_current_tcb_ptr
    str r2, [r3]                /* g_current_tcb_ptr = g_next_tcb_ptr */

    /* --- 动态更新特权级 (CONTROL.nPRIV) --- */
    /* M0+ 无 bfi，手动位操作：
     * r3 = new privilege (0 or 1)
     * control = (control & ~1) | r3
     */
    ldr r3, [r2, #4]            /* r3 = tcb_next->privilege */
    mrs r0, control
    movs r1, #1
    bics r0, r1                 /* 清除 bit 0 */
    orrs r0, r3                 /* 设置 bit 0 = privilege */
    msr control, r0
    nop                         /* 等效 ISB（M0+ 无 ISB）*/

    /* --- 动态更新 MPU 沙盒（M0+ 暂为空操作）--- */
    push {r2, r3}               /* 保存 r2（next_tcb）和 r3（临时）*/
    mov r3, lr                  /* 保存 lr 到 r3 */
    push {r3}
    mov r0, r2                  /* r0 = next_tcb */
    bl mpu_switch_sandbox
    pop {r3}
    mov lr, r3                  /* 恢复 lr */
    pop {r2, r3}

    /* --- 恢复新线程 r4-r7 --- */
    ldr r0, [r2]                /* r0 = tcb_next->stack_ptr */
    ldr r7, [r0, #12]
    ldr r6, [r0, #8]
    ldr r5, [r0, #4]
    ldr r4, [r0, #0]
    adds r0, r0, #16            /* 跳过已弹出的 r4-r7 */

    msr psp, r0                 /* 更新 PSP */

    /* --- Metrics hook --- */
    mov r3, lr                  /* 保存 lr 到 r3 */
    push {r3}
    bl pendsv_metrics_hook
    pop {r3}
    mov lr, r3                  /* 恢复 lr */

    cpsie i                     /* 2. 开中断 */
    bx lr                       /* 3. 硬件弹出剩余寄存器，跳转新线程 */

/* ================================================================
 * SVC_Handler — 系统调用入口
 *
 * ARMv6-M 无 ITE 条件块，用两次独立分支替代：
 *   tst lr, #4  →  EQ: 用 MSP  /  NE: 用 PSP
 * ================================================================ */
.global SVC_Handler
.thumb_func
.type SVC_Handler, %function
SVC_Handler:
    movs r1, #4                 /* r1 = 4 */
    mov r2, lr                  /* 将 lr (r14) 移到低寄存器 r2 */
    ands r1, r2                 /* r1 = lr & 4 */
    bne use_psp                 /* 如果不为 0，用 PSP */
    mrs r0, msp                 /* bit2=0 → 使用 MSP */
    b svc_dispatch
use_psp:
    mrs r0, psp                 /* bit2=1 → 使用 PSP */
svc_dispatch:
    b SVC_Handler_C             /* 跳转 C 分发函数，r0 = 栈帧指针 */
