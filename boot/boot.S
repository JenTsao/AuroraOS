#include "config/autoconf.h"

.syntax unified
.cpu cortex-m4
.thumb

.global _start
.global Reset_Handler

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

    .thumb_func
    .weak Default_Handler
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

#ifndef CONFIG_BOARD_MIBAND8
.global PendSV_Handler
.thumb_func
.type PendSV_Handler, %function
PendSV_Handler:
    cpsid i                     @ 1. 切换期间关闭中断，保证安全
    
    @ --- 保存当前旧线程的上下文 ---
    mrs r0, psp                 @ 获取当前运行线程的线程栈指针 (PSP)
    
    @ 手动将 r4-r11 压入旧线程的栈中 (Cortex-M4 硬件不自动压这几个)
    stmdb r0!, {r4-r11}
    
    @ 将更新后的栈指针 (SP) 写入旧线程的 TCB
    ldr r1, =g_current_tcb_ptr
    ldr r1, [r1]                @ r1 = *g_current_tcb_ptr
    str r0, [r1]                @ TCB 的第一个成员就是 stack_ptr，等同于 tcb->stack_ptr = r0

    @ --- 切换 TCB 指针 ---
    ldr r2, =g_next_tcb_ptr
    ldr r2, [r2]                @ r2 = *g_next_tcb_ptr
    
    ldr r3, =g_current_tcb_ptr
    str r2, [r3]                @ *g_current_tcb_ptr = *g_next_tcb_ptr (同步 C++ 的选择)

    @ --- 动态更新特权级 (CONTROL.nPRIV) ---
    ldr r3, [r2, #4]            @ 读取新任务的 privilege (TCB 偏移 4)
    mrs r0, control
    bfi r0, r3, #0, #1          @ 将 r3 的最低位插入 control 的第 0 位 (nPRIV)
    msr control, r0
    isb                         @ 指令同步屏障

    @ --- 动态更新 MPU 沙盒 ---
    push {r2, lr}               @ 保存 r2 (新 TCB 指针) 和 lr (EXC_RETURN)，8字节对齐
    mov r0, r2                  @ r0 = 参数 next_tcb
    bl mpu_switch_sandbox       @ 调用 C 函数 mpu_switch_sandbox
    pop {r2, lr}                @ 恢复 r2 和 lr

    @ --- 恢复新线程的上下文 ---
    ldr r0, [r2]                @ r0 = tcb_next->stack_ptr (获取新线程的栈顶)

    @ 从新线程的栈中弹出 r4-r11 恢复给 CPU
    ldmia r0!, {r4-r11}
    
    msr psp, r0                 @ 将新栈顶指针恢复给线程栈指针 (PSP)
    
    push {lr}
    bl pendsv_metrics_hook
    pop {lr}
    
    cpsie i                     @ 2. 重新开启全局中断
    bx lr                       @ 3. 硬件自动弹出剩下寄存器，并跳转回新线程的 PC 处继续执行！
#endif


.global SVC_Handler
.thumb_func
.type SVC_Handler, %function
SVC_Handler:
    @ 判断异常前使用的是 MSP 还是 PSP
    tst lr, #4
    ite eq
    mrseq r0, msp
    mrsne r0, psp
    @ 跳转到 C 函数，r0 即为传入的上下文帧指针
    b SVC_Handler_C
