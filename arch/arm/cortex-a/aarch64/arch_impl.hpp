#ifndef ARCH_AARCH64_IMPL_HPP
#define ARCH_AARCH64_IMPL_HPP

#include <stdint.h>
#include <stddef.h>

namespace Arch {

inline void disable_interrupts() {
#if defined(__aarch64__) || defined(ARCH_AARCH64)
    __asm__ volatile("msr daifset, #2" : : : "memory");
#endif
}

inline void enable_interrupts() {
#if defined(__aarch64__) || defined(ARCH_AARCH64)
    __asm__ volatile("msr daifclr, #2" : : : "memory");
#endif
}

inline uint32_t irq_save() {
    uint32_t flags = 0;
#if defined(__aarch64__) || defined(ARCH_AARCH64)
    __asm__ volatile("mrs %0, daif \n\t"
                     "msr daifset, #2 \n\t"
                     : "=r"(flags)
                     :
                     : "memory");
#endif
    return flags;
}

inline void irq_restore(uint32_t flags) {
#if defined(__aarch64__) || defined(ARCH_AARCH64)
    __asm__ volatile("msr daif, %0 \n\t" : : "r"(flags) : "memory");
#else
    (void)flags;
#endif
}

inline void wait_for_interrupt() {
#if defined(__aarch64__) || defined(ARCH_AARCH64)
    __asm__ volatile("wfi" : : : "memory");
#endif
}

inline uint32_t get_cycle() {
#if defined(__aarch64__) || defined(ARCH_AARCH64)
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return static_cast<uint32_t>(val);
#else
    return 0;
#endif
}

inline uint32_t get_cycles_per_us() {
#if defined(__aarch64__) || defined(ARCH_AARCH64)
    uint64_t frq = 62500000; // Typical 62.5MHz on QEMU virt
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    return static_cast<uint32_t>(frq / 1000000);
#else
    return 62;
#endif
}

// ── Generic Timer (SysTick equivalent for ARMv8-A) ───────────────────────────

inline void systick_init(uint32_t hz) {
#if defined(__aarch64__) || defined(ARCH_AARCH64)
    uint64_t cntfrq = 62500000;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
    uint32_t count = static_cast<uint32_t>(cntfrq / hz);
    __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(static_cast<uint64_t>(count)) : "memory");
    // Enable timer (bit 0 = ENABLE, bit 1 = IMASK 0 = unmasked)
    uint64_t ctl = 1;
    __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(ctl) : "memory");
#else
    (void)hz;
#endif
}

inline void disable_systick() {
#if defined(__aarch64__) || defined(ARCH_AARCH64)
    uint64_t ctl = 0;
    __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(ctl) : "memory");
#endif
}

inline void enable_systick() {
#if defined(__aarch64__) || defined(ARCH_AARCH64)
    uint64_t ctl = 1;
    __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(ctl) : "memory");
#endif
}

inline void start_wakeup_timer(uint32_t ticks) {
#if defined(__aarch64__) || defined(ARCH_AARCH64)
    uint64_t cntfrq = 62500000;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
    uint64_t count = (cntfrq / 1000) * ticks;
    __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(count) : "memory");
    enable_systick();
#else
    (void)ticks;
#endif
}

inline uint32_t stop_wakeup_timer() {
    return 0;
}

inline void trigger_context_switch() {
#if defined(__aarch64__) || defined(ARCH_AARCH64)
    __asm__ volatile("svc #0" : : : "memory");
#endif
}

inline void switch_address_space(uintptr_t pgdir_base) {
#if defined(__aarch64__) || defined(ARCH_AARCH64)
    if (pgdir_base != 0) {
        __asm__ volatile("msr ttbr0_el1, %0 \n\t"
                         "isb \n\t"
                         "tlbi vmalle1is \n\t"
                         "dsb sy \n\t"
                         "isb \n\t" ::"r"(pgdir_base)
                         : "memory");
    }
#else
    (void)pgdir_base;
#endif
}

inline uint32_t* init_thread_stack(void (*task_entry)(void), uint32_t* stack_space, uint32_t stack_size) {
    uintptr_t top = reinterpret_cast<uintptr_t>(stack_space) + stack_size;
    top &= ~0xFULL; // 16-byte alignment required by AArch64 ABI

    // Frame layout (272 bytes = 34 * 8 bytes):
    // [0..239]   x0..x29
    // [240..247] x30 (LR)
    // [248..255] elr_el1 (PC)
    // [256..263] spsr_el1
    // [264..271] sp_el0
    top -= 272;
    uint64_t* frame = reinterpret_cast<uint64_t*>(top);
    for (int i = 0; i < 34; ++i) {
        frame[i] = 0;
    }

    frame[30] = reinterpret_cast<uint64_t>(task_entry); // LR (x30)
    frame[31] = reinterpret_cast<uint64_t>(task_entry); // ELR_EL1 (entry PC)
    frame[32] = 0x00000005;                             // SPSR_EL1: EL1h (M[3:0]=0101b, interrupts enabled)
    frame[33] = top + 272;                              // SP_EL0

    return reinterpret_cast<uint32_t*>(top);
}

[[noreturn]] inline void start_first_task(uint32_t* stack_ptr, void (*entry_point)(), uint32_t privilege = 0) {
    (void)entry_point;
    (void)privilege;
#if defined(__aarch64__) || defined(ARCH_AARCH64)
    __asm__ volatile("mov sp, %0 \n\t"
                     "ldp x23, x24, [sp, #256] \n\t"
                     "msr spsr_el1, x23 \n\t"
                     "msr sp_el0, x24 \n\t"
                     "ldp x30, x22, [sp, #240] \n\t"
                     "msr elr_el1, x22 \n\t"
                     "ldp x28, x29, [sp, #224] \n\t"
                     "ldp x26, x27, [sp, #208] \n\t"
                     "ldp x24, x25, [sp, #192] \n\t"
                     "ldp x22, x23, [sp, #176] \n\t"
                     "ldp x20, x21, [sp, #160] \n\t"
                     "ldp x18, x19, [sp, #144] \n\t"
                     "ldp x16, x17, [sp, #128] \n\t"
                     "ldp x14, x15, [sp, #112] \n\t"
                     "ldp x12, x13, [sp, #96] \n\t"
                     "ldp x10, x11, [sp, #80] \n\t"
                     "ldp x8, x9, [sp, #64] \n\t"
                     "ldp x6, x7, [sp, #48] \n\t"
                     "ldp x4, x5, [sp, #32] \n\t"
                     "ldp x2, x3, [sp, #16] \n\t"
                     "ldp x0, x1, [sp, #0] \n\t"
                     "add sp, sp, #272 \n\t"
                     "eret \n\t"
                     :
                     : "r"(stack_ptr));
#else
    (void)stack_ptr;
#endif
    while (1)
        ;
}

inline void set_privilege(uint32_t /*privilege*/) {}

inline void mpu_configure_region(uint8_t /*idx*/, const MpuRegion& /*region*/) noexcept {}
inline void mpu_enable() noexcept {}
inline void mpu_disable() noexcept {}

} // namespace Arch

#endif // ARCH_AARCH64_IMPL_HPP
