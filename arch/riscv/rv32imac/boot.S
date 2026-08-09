.global _start
.global _stext
.global _etext
.global _sdata
.global _edata
.global _sbss
.global _ebss
.global _estack
.global __init_array_start
.global __init_array_end

.section .init, "ax"
_start:
    /* Setup stack */
    la sp, _estack

    /* Load trap vector */
    la t0, trap_vector
    csrw mtvec, t0

    /* Clear BSS */
    la a0, _sbss
    la a1, _ebss
    bge a0, a1, bss_done
bss_loop:
    sw zero, 0(a0)
    addi a0, a0, 4
    blt a0, a1, bss_loop
bss_done:

    /* Copy Data */
    la a0, _sidata
    la a1, _sdata
    la a2, _edata
    bge a1, a2, data_done
data_loop:
    lw t0, 0(a0)
    sw t0, 0(a1)
    addi a0, a0, 4
    addi a1, a1, 4
    blt a1, a2, data_loop
data_done:

    /* Run C++ global static constructors (.init_array).
     * Without this, any global object whose constructor cannot be
     * constant-folded into .data (e.g. one that sets up a vtable)
     * is left as zero-filled .bss garbage — silently, with no
     * boot-time indication of the failure. */
    la s0, __init_array_start
    la s1, __init_array_end
    bge s0, s1, init_array_done
init_array_loop:
    lw t0, 0(s0)
    jalr ra, t0, 0
    addi s0, s0, 4
    blt s0, s1, init_array_loop
init_array_done:

    /* Enable global interrupts */
    /* Wait, start_first_task will enable interrupts via mret */
    
    /* Call kernel_main */
    call kernel_main

halt:
    wfi
    j halt