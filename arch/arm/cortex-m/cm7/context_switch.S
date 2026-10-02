/*
 * auroraOS Cortex-M7 (ARMv7E-M) 上下文切换引擎
 *
 * 切换语义 = cm4f 方案 (FPv5 惰性压栈) + cm3 路径的沙盒/metrics 钩子:
 *   - EXC_RETURN bit4 == 0 表示任务已激活 FPU, 需保存/恢复 S16-S31
 *     (S0-S15 与 FPSCR 由异常进入/返回时的硬件自动压栈/出栈,
 *      FPCCR.ASPEN|LSPEN 已在 early_init 中开启)
 *   - 特权级从新任务 TCB 偏移 4 处读取, 写入 CONTROL.nPRIV
 *   - mpu_switch_sandbox(): 按任务切换 MPU 用户沙盒 (boot/interrupts.cpp)
 *   - pendsv_metrics_hook(): 上下文切换耗时度量采样
 */
.syntax unified
.cpu cortex-m7
.fpu fpv5-sp-d16
.thumb

.global PendSV_Handler
.extern g_current_tcb_ptr       /* C++ 全局: 当前运行 TCB */
.extern g_next_tcb_ptr          /* C++ 全局: 即将运行 TCB */
.extern mpu_switch_sandbox
.extern pendsv_metrics_hook

.section .text
.thumb_func
.align 2
PendSV_Handler:
    cpsid i                     /* 切换期间关全局中断, 保证原子性 */

    /* ============ 1. 挂起当前任务 (Save Context) ============ */
    mrs r0, psp
    cbz r0, restore_context     /* PSP==0: 系统刚启动, 无旧上下文可存 */

    tst lr, #0x10               /* EXC_RETURN bit4==0: FPU 已激活 */
    it eq
    vstmdbeq r0!, {s16-s31}     /* 压入高半区浮点寄存器 (32 bytes) */

    stmdb r0!, {r4-r11, lr}     /* R4-R11 + EXC_RETURN 一并压栈 */

    ldr r1, =g_current_tcb_ptr
    ldr r2, [r1]
    str r0, [r2, #12]           /* [tcb+12] = task.stack_ptr (PendSV ABI, 见 task.hpp 契约) */

    /* ============ 2. 恢复下一个任务 (Restore Context) ============ */
restore_context:
    ldr r1, =g_current_tcb_ptr
    ldr r2, =g_next_tcb_ptr
    ldr r2, [r2]
    str r2, [r1]                /* g_current_tcb_ptr = g_next_tcb_ptr */

    /* 动态更新特权级: [tcb+16] 处的 privilege 位插入 CONTROL.nPRIV */
    ldr r3, [r2, #16]
    mrs r0, control
    bfi r0, r3, #0, #1
    msr control, r0
    isb

    /* 切换 MPU 用户沙盒 (C 调用约定: r0 = next_tcb; r2/lr 需要保护) */
    push {r2, lr}
    mov r0, r2
    bl mpu_switch_sandbox
    pop {r2, lr}

    ldr r0, [r2, #12]           /* r0 = next_tcb->task.stack_ptr */
    ldmia r0!, {r4-r11, lr}     /* 弹出 R4-R11 与新任务的 EXC_RETURN */

    tst lr, #0x10               /* 新任务是否需要恢复 FPU 上下文 */
    it eq
    vldmiaeq r0!, {s16-s31}

    msr psp, r0

    /* ============ 3. 切换度量采样后点火 ============ */
    push {lr}
    bl pendsv_metrics_hook
    pop {lr}

    cpsie i
    bx lr                       /* 硬件自动弹出 R0-R3/PC, 切入线程模式 */
