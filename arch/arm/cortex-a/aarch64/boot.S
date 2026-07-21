    .section .text.boot
    .global _start

_start:
    // Read CPU ID, stop slave cores
    mrs     x1, mpidr_el1
    and     x1, x1, #3
    cbz     x1, 2f
1:  wfe
    b       1b
2:
    // Setup stack
    ldr     x1, =_start
    mov     sp, x1

    // Clear BSS
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
    mov     x3, #0
3:  cmp     x1, x2
    b.eq    4f
    str     xzr, [x1], #8
    b       3b
4:
    // Set up exception vector table
    ldr     x0, =vector_table
    msr     vbar_el1, x0

    // Jump to kernel main
    bl      kernel_main
    b       1b
