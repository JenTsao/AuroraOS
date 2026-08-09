/* 
 * auroraOS MiBand 8 (Cortex-M4F) 核心上下文切换引擎
 * 采用 GNU AS 语法，完美适配 CMake/GCC 工具链
 */

.syntax unified
.cpu cortex-m4
.fpu fpv4-sp-d16
.thumb

.global PendSV_Handler
.extern g_current_tcb_ptr  /* 声明外部 C++ 全局指针：当前运行的 TCB */
.extern g_next_tcb_ptr     /* 声明外部 C++ 全局指针：即将运行的 TCB */

.section .text
.thumb_func
.align 2
PendSV_Handler:
    /* ======================================================== */
    /* 1. 挂起当前任务 (Save Context)                             */
    /* ======================================================== */
    cpsid i                 /* 关全局中断，确保上下文切换的原子性 */

    mrs r0, psp             /* 将当前进程栈指针 (PSP) 提取到 R0 */
    cbz r0, restore_context /* 如果 PSP 为 0，说明是系统刚启动，直接跳到恢复阶段 */

    /* 判断当前任务是否使用了 FPU 浮点单元 */
    /* LR (R14) 中存储了 EXC_RETURN。如果它的 Bit 4 为 0，说明 FPU 处于激活状态 */
    tst lr, #0x10           
    it eq                   /* IF-THEN 汇编指令，如果 Bit 4 == 0 (EQ) */
    vstmdbeq r0!, {s16-s31} /* 将剩余的浮点寄存器 S16-S31 压入当前任务的 PSP 栈中 */

    /* 保存常规寄存器 R4-R11 以及关键的 EXC_RETURN (LR) */
    stmdb r0!, {r4-r11, lr} 

    /* 将最新的栈顶地址 (R0) 保存回当前任务的 TCB 控制块中 */
    ldr r1, =g_current_tcb_ptr
    ldr r2, [r1]            /* R2 = g_current_tcb_ptr */
    str r0, [r2]            /* g_current_tcb_ptr->stack_ptr = R0 */

    /* ======================================================== */
    /* 2. 恢复下一个任务 (Restore Context)                        */
    /* ======================================================== */
restore_context:
    /* 切换指针：g_current_tcb_ptr = g_next_tcb_ptr */
    ldr r1, =g_current_tcb_ptr
    ldr r2, =g_next_tcb_ptr
    ldr r3, [r2]
    str r3, [r1]

    /* 动态更新特权级 (CONTROL.nPRIV) */
    ldr r1, [r3, #4]            /* 读取新任务的 privilege (TCB 偏移 4) */
    mrs r0, control
    bfi r0, r1, #0, #1          /* 将 r1 的最低位插入 control 的第 0 位 */
    msr control, r0
    isb                         /* 指令同步屏障 */

    /* 从新任务的 TCB 中加载栈顶指针到 R0 */
    ldr r0, [r3]            /* R0 = g_next_tcb_ptr->stack_ptr */

    /* 从新任务的栈中弹出常规寄存器 R4-R11，并恢复其 EXC_RETURN (LR) */
    ldmia r0!, {r4-r11, lr}

    /* 检查即将运行的新任务是否需要恢复 FPU 上下文 */
    /* 判断刚刚弹出的 LR (EXC_RETURN) 的 Bit 4 */
    tst lr, #0x10
    it eq                   
    vldmiaeq r0!, {s16-s31} /* 如果需要，将 S16-S31 弹出恢复到 FPU 硬件中 */

    /* 将游标更新后的 R0 重新写入硬件的进程栈指针 (PSP) */
    msr psp, r0

    /* ======================================================== */
    /* 3. 点火起飞                                              */
    /* ======================================================== */
    cpsie i                 /* 开全局中断 */
    bx lr                   /* 携带特殊的 EXC_RETURN 返回，硬件会自动弹出 R0-R3, PC 等，并切入线程模式！ */
