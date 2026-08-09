    .section .vectors, "ax"
    .balign 2048
    .global vector_table

.macro  ventry  label
    .balign 128
    b       \label
.endm

vector_table:
    // Current EL with SP0
    ventry  sync_invalid_el1t
    ventry  irq_invalid_el1t
    ventry  fiq_invalid_el1t
    ventry  error_invalid_el1t

    // Current EL with SPx
    ventry  sync_invalid_el1h
    ventry  irq_invalid_el1h
    ventry  fiq_invalid_el1h
    ventry  error_invalid_el1h

    // Lower EL using AArch64
    ventry  sync_invalid_el0_64
    ventry  irq_invalid_el0_64
    ventry  fiq_invalid_el0_64
    ventry  error_invalid_el0_64

    // Lower EL using AArch32
    ventry  sync_invalid_el0_32
    ventry  irq_invalid_el0_32
    ventry  fiq_invalid_el0_32
    ventry  error_invalid_el0_32

// Basic handler stub
.macro handle_invalid entry
\entry:
    b       \entry
.endm

.macro save_context
    sub     sp, sp, #272    // 34 * 8 = 272 bytes (x0-x30, elr, spsr, sp_el0)
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    mrs     x22, elr_el1
    mrs     x23, spsr_el1
    mrs     x24, sp_el0
    stp     x30, x22, [sp, #240] // x30 (LR) and elr_el1
    stp     x23, x24, [sp, #256] // spsr_el1 and sp_el0
.endm

.macro restore_context
    ldp     x23, x24, [sp, #256]
    msr     spsr_el1, x23
    msr     sp_el0, x24
    ldp     x30, x22, [sp, #240]
    msr     elr_el1, x22
    ldp     x28, x29, [sp, #224]
    ldp     x26, x27, [sp, #208]
    ldp     x24, x25, [sp, #192]
    ldp     x22, x23, [sp, #176]
    ldp     x20, x21, [sp, #160]
    ldp     x18, x19, [sp, #144]
    ldp     x16, x17, [sp, #128]
    ldp     x14, x15, [sp, #112]
    ldp     x12, x13, [sp, #96]
    ldp     x10, x11, [sp, #80]
    ldp     x8, x9, [sp, #64]
    ldp     x6, x7, [sp, #48]
    ldp     x4, x5, [sp, #32]
    ldp     x2, x3, [sp, #16]
    ldp     x0, x1, [sp, #0]
    add     sp, sp, #272
.endm

// === P0/P1 Handlers ===

// 1. User mode IRQ (Lower EL, AArch64)
irq_invalid_el0_64:
    save_context
    mov     x0, sp
    bl      irq_handler_c
    restore_context
    eret

// 2. Kernel mode IRQ (Current EL, SPx)
irq_invalid_el1h:
    save_context
    mov     x0, sp
    bl      irq_handler_c
    restore_context
    eret

// 3. User mode SVC (Lower EL, AArch64)
sync_invalid_el0_64:
    save_context
    mrs     x0, esr_el1
    lsr     x1, x0, #26       // EC
    cmp     x1, #0x15         // SVC in AArch64
    b.ne    1f
    mov     x0, sp
    bl      svc_handler_c
    restore_context
    eret
1:  // If not SVC, it's another sync exception
    mov     x0, sp
    bl      sync_handler_c
    restore_context
    eret

// 4. Kernel mode Sync Exception (Current EL, SPx)
sync_invalid_el1h:
    save_context
    mov     x0, sp
    bl      sync_handler_c
    restore_context
    eret

// 5. User mode SError (Lower EL, AArch64)
error_invalid_el0_64:
    save_context
    mov     x0, sp
    bl      error_handler_c
    restore_context
    eret

// === Unhandled Exceptions (Infinite Loop) ===
handle_invalid sync_invalid_el1t
handle_invalid irq_invalid_el1t
handle_invalid fiq_invalid_el1t
handle_invalid error_invalid_el1t

handle_invalid fiq_invalid_el1h
handle_invalid error_invalid_el1h

handle_invalid fiq_invalid_el0_64

handle_invalid sync_invalid_el0_32
handle_invalid irq_invalid_el0_32
handle_invalid fiq_invalid_el0_32
handle_invalid error_invalid_el0_32
