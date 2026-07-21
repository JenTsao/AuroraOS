// Minimal boot/startup code for the secure-boot bootloader (lm3s6965-qb / QEMU).
//
// The bootloader does NOT use the kernel scheduler, SVC calls, MPU sandboxing
// or the SysTick timer, so it must not link the kernel's boot.S (which pulls in
// kernel-only symbols such as mpu_switch_sandbox, SVC_Handler_C, g_current_tcb_ptr,
// pendsv_metrics_hook, SysTick_Handler, etc.).
//
// This file keeps the same vector-table layout as boot/boot.S (so exception offsets
// remain correct) but wires every handler to a weak Default_Handler. The bootloader
// never enables SVC/PendSV/SysTick, so those handlers are never invoked.

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

    .weak SVC_Handler
    .thumb_set SVC_Handler, Default_Handler

    .weak DebugMon_Handler
    .thumb_set DebugMon_Handler, Default_Handler

    .weak PendSV_Handler
    .thumb_set PendSV_Handler, Default_Handler

    .weak SysTick_Handler
    .thumb_set SysTick_Handler, Default_Handler
