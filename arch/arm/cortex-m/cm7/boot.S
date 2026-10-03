/*
 * auroraOS Cortex-M7 (ARMv7E-M) 启动代码与向量表 (QEMU MPS2+ AN500)
 *
 * 与 cm4f 的差异:
 *   - FPU 与 I/D-Cache 的使能在 arch_early_init() (cm7/early_init.cpp) 中,
 *     Reset_Handler 在进入 kernel_main 前调用, 保证硬浮点 ABI 下任何浮点
 *     指令执行前 FPU 已就绪
 *   - 本文件不定义 PendSV_Handler: FPU 感知的上下文切换引擎独占实现于
 *     cm7/context_switch.S, 避免弱/强符号解析歧义
 *
 * 复位后内存布局由链接脚本提供: _sidata/_sdata/_edata/_sbss/_ebss/_estack
 */
.syntax unified
.cpu cortex-m7
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
    .word PendSV_Handler            @ 实现于 cm7/context_switch.S
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
    bge arch_init
    str r2, [r0]
    adds r0, r0, #4
    b zero_loop

arch_init:
    bl arch_early_init              @ FPU + I/D-Cache 使能 (cm7/early_init.cpp)

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

.global SVC_Handler
.thumb_func
.type SVC_Handler, %function
SVC_Handler:
    @ 判断异常前使用的是 MSP 还是 PSP, 将对应栈指针作为上下文帧传入 C 分发器
    tst lr, #4
    ite eq
    mrseq r0, msp
    mrsne r0, psp
    b SVC_Handler_C
