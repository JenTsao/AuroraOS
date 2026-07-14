#ifndef ARCH_IMPL_HPP
#define ARCH_IMPL_HPP

#include <stdint.h>
#include "board.h"

namespace Arch {
    constexpr uintptr_t CLINT_BASE = BOARD_CLINT_BASE;
    constexpr uintptr_t CLINT_MSIP = CLINT_BASE + 0x0000;
    constexpr uintptr_t CLINT_MTIMECMP = CLINT_BASE + 0x4000;
    constexpr uintptr_t CLINT_MTIME  = CLINT_BASE + 0xBFF8;

    inline void disable_interrupts() {
        __asm__ volatile ("csrc mstatus, %0" : : "r"(8) : "memory");
    }

    inline void enable_interrupts() {
        __asm__ volatile ("csrs mstatus, %0" : : "r"(8) : "memory");
    }

    inline uint32_t irq_save() {
        uint32_t flags;
        __asm__ volatile (
            "csrr %0, mstatus \n\t"
            "csrc mstatus, %1"
            : "=r" (flags)
            : "r" (8)
            : "memory"
        );
        return flags;
    }

    inline void irq_restore(uint32_t flags) {
        __asm__ volatile (
            "csrw mstatus, %0"
            :
            : "r" (flags)
            : "memory"
        );
    }

    inline void wait_for_interrupt() {
        __asm__ volatile ("wfi" : : : "memory");
    }

    inline uint32_t get_cycle() {
        return *reinterpret_cast<volatile uint32_t*>(CLINT_MTIME);
    }

    inline uint32_t get_cycles_per_us() {
        // Typical QEMU virt CLINT frequency is 10MHz
        return 10;
    }

    inline uint64_t get_mtime() {
        volatile uint32_t* mtime = reinterpret_cast<volatile uint32_t*>(CLINT_MTIME);
        uint32_t hi, lo;
        do {
            hi = mtime[1];
            lo = mtime[0];
        } while (hi != mtime[1]);
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }

    inline void systick_init(uint32_t hz) {
        // Enable Machine Timer Interrupt (MTIE = bit 7 in mie)
        uint32_t mtie = (1 << 7);
        __asm__ volatile ("csrs mie, %0" : : "r"(mtie) : "memory");

        uint64_t now = get_mtime();
        uint32_t interval = (get_cycles_per_us() * 1000000) / hz;
        uint64_t target = now + interval;
        
        volatile uint32_t* mtimecmp = reinterpret_cast<volatile uint32_t*>(CLINT_MTIMECMP);
        mtimecmp[0] = 0xFFFFFFFF; // Prevent spurious
        mtimecmp[1] = static_cast<uint32_t>(target >> 32);
        mtimecmp[0] = static_cast<uint32_t>(target & 0xFFFFFFFF);
    }

    inline void disable_systick() {
        uint32_t mtie = (1 << 7);
        __asm__ volatile ("csrc mie, %0" : : "r"(mtie) : "memory");
    }

    inline void enable_systick() {
        uint32_t mtie = (1 << 7);
        __asm__ volatile ("csrs mie, %0" : : "r"(mtie) : "memory");
    }

    inline uint64_t sleep_start_mtime = 0;

    inline void start_wakeup_timer(uint32_t ticks) {
        sleep_start_mtime = get_mtime();
        
        uint32_t hz = 1000; // Assuming OS Tick is 1ms
        uint32_t interval = (get_cycles_per_us() * 1000000) / hz;
        uint64_t target = sleep_start_mtime + (static_cast<uint64_t>(interval) * ticks);
        
        volatile uint32_t* mtimecmp = reinterpret_cast<volatile uint32_t*>(CLINT_MTIMECMP);
        mtimecmp[0] = 0xFFFFFFFF; 
        mtimecmp[1] = static_cast<uint32_t>(target >> 32);
        mtimecmp[0] = static_cast<uint32_t>(target & 0xFFFFFFFF);
    }

    inline uint32_t stop_wakeup_timer() {
        uint64_t wake_mtime = get_mtime();
        uint32_t hz = 1000;
        uint32_t interval = (get_cycles_per_us() * 1000000) / hz;
        
        uint64_t elapsed = wake_mtime - sleep_start_mtime;
        
        // Restore next normal tick
        uint64_t target = wake_mtime + interval;
        volatile uint32_t* mtimecmp = reinterpret_cast<volatile uint32_t*>(CLINT_MTIMECMP);
        mtimecmp[0] = 0xFFFFFFFF; 
        mtimecmp[1] = static_cast<uint32_t>(target >> 32);
        mtimecmp[0] = static_cast<uint32_t>(target & 0xFFFFFFFF);
        
        return static_cast<uint32_t>(elapsed / interval);
    }

    inline void trigger_context_switch() {
        // Trigger Machine Software Interrupt
        *reinterpret_cast<volatile uint32_t*>(CLINT_MSIP) = 1;
    }

    inline uint32_t* init_thread_stack(void (*task_entry)(void), uint32_t* stack_space, uint32_t stack_size) {
        uint32_t* top = stack_space + (stack_size / sizeof(uint32_t));
        
        // Trap frame: 33 words (132 bytes)
        // [0..30] GPRs x1-x31
        // [31] mepc
        // [32] mstatus
        top -= 33;
        
        top[32] = 0x1880; // mstatus: MPP=3 (M-mode), MPIE=1
        top[31] = reinterpret_cast<uint32_t>(task_entry); // mepc
        
        // Zero init x1-x31
        for(int i = 0; i <= 30; i++) top[i] = 0;
        
        return top;
    }

    [[noreturn]] inline void start_first_task(uint32_t* stack_ptr, void (*entry_point)(), uint32_t privilege = 0) {
        __asm__ volatile (
            "mv sp, %0 \n\t"
            // Load mepc and mstatus
            "lw t0, 124(sp) \n\t"
            "csrw mepc, t0 \n\t"
            "lw t0, 128(sp) \n\t"
            "csrw mstatus, t0 \n\t"
            // Restore context GPRs (x1-x31)
            "lw x1, 0(sp) \n\t"
            "lw x3, 8(sp) \n\t"
            "lw x4, 12(sp) \n\t"
            "lw x5, 16(sp) \n\t"
            "lw x6, 20(sp) \n\t"
            "lw x7, 24(sp) \n\t"
            "lw x8, 28(sp) \n\t"
            "lw x9, 32(sp) \n\t"
            "lw x10, 36(sp) \n\t"
            "lw x11, 40(sp) \n\t"
            "lw x12, 44(sp) \n\t"
            "lw x13, 48(sp) \n\t"
            "lw x14, 52(sp) \n\t"
            "lw x15, 56(sp) \n\t"
            "lw x16, 60(sp) \n\t"
            "lw x17, 64(sp) \n\t"
            "lw x18, 68(sp) \n\t"
            "lw x19, 72(sp) \n\t"
            "lw x20, 76(sp) \n\t"
            "lw x21, 80(sp) \n\t"
            "lw x22, 84(sp) \n\t"
            "lw x23, 88(sp) \n\t"
            "lw x24, 92(sp) \n\t"
            "lw x25, 96(sp) \n\t"
            "lw x26, 100(sp) \n\t"
            "lw x27, 104(sp) \n\t"
            "lw x28, 108(sp) \n\t"
            "lw x29, 112(sp) \n\t"
            "lw x30, 116(sp) \n\t"
            "lw x31, 120(sp) \n\t"
            // Pop stack frame
            "addi sp, sp, 132 \n\t"
            "mret \n\t"
            : : "r"(stack_ptr)
        );
        __builtin_unreachable();
    }

    // =====================================================================
    // RISC-V PMP (Physical Memory Protection) 实现
    //
    // PMP 在 RV32 上最多支持 16 个区域 (pmpcfg0-3 / pmpaddr0-15)
    // pmpcfg寄存器内局: [L|00|A|X|W|R]
    //   A=0(OFF) A=2(NA4,4字节对齐) A=3(NAPOT,2的幂对齐)
    // NAPOT 地址编码: pmpaddr = (base >> 2) | ((1 << (size_pow2-3)) - 1)
    //
    // 外部接口与 ARM MPU 相同（MpuRegion::ap 字段展5取低3位用作 R/W/X权限组合）
    // ap bit2 = X allow, bit1 = W allow, bit0 = R allow
    // =====================================================================

    // 写单个 pmpcfg 字节 (富8个pmpaddr寄存器共用pmpcfg0-3)
    inline void _pmp_set_cfg(uint8_t idx, uint8_t cfg_byte) noexcept {
        uint32_t reg_val;
        uint8_t shift = (idx & 3u) * 8u;
        // RV32: pmpcfg0=idx0-3, pmpcfg1=idx4-7, etc.
        switch (idx >> 2) {
            case 0: __asm__ volatile ("csrr %0, pmpcfg0" : "=r"(reg_val)); break;
            case 1: __asm__ volatile ("csrr %0, pmpcfg1" : "=r"(reg_val)); break;
            case 2: __asm__ volatile ("csrr %0, pmpcfg2" : "=r"(reg_val)); break;
            case 3: __asm__ volatile ("csrr %0, pmpcfg3" : "=r"(reg_val)); break;
            default: return;
        }
        reg_val &= ~(0xFFu << shift);
        reg_val |= (static_cast<uint32_t>(cfg_byte) << shift);
        switch (idx >> 2) {
            case 0: __asm__ volatile ("csrw pmpcfg0, %0" : : "r"(reg_val)); break;
            case 1: __asm__ volatile ("csrw pmpcfg1, %0" : : "r"(reg_val)); break;
            case 2: __asm__ volatile ("csrw pmpcfg2, %0" : : "r"(reg_val)); break;
            case 3: __asm__ volatile ("csrw pmpcfg3, %0" : : "r"(reg_val)); break;
        }
    }

    inline void mpu_configure_region(uint8_t idx, const MpuRegion& r) noexcept {
        if (idx >= 16u) return; // QEMU virt 最多 16 张 PMP

        // NAPOT 地址编码: (base >> 2) | ((size/2 - 1) >> 2)
        // size = 2^size_pow2, NAPOT要求 size >= 8 字节
        uint32_t napot_addr;
        if (r.size_pow2 >= 3u) {
            napot_addr = (static_cast<uint32_t>(r.base) >> 2) |
                         ((1u << (r.size_pow2 - 3u)) - 1u);
        } else {
            napot_addr = static_cast<uint32_t>(r.base) >> 2; // NA4 fallback
        }

        // 写 pmpaddr，需要先用内联汇编按索引选择
        // RISC-V 没有间接寄存器地址，只能用 switch
        switch (idx) {
            case  0: __asm__ volatile ("csrw pmpaddr0,  %0" : : "r"(napot_addr)); break;
            case  1: __asm__ volatile ("csrw pmpaddr1,  %0" : : "r"(napot_addr)); break;
            case  2: __asm__ volatile ("csrw pmpaddr2,  %0" : : "r"(napot_addr)); break;
            case  3: __asm__ volatile ("csrw pmpaddr3,  %0" : : "r"(napot_addr)); break;
            case  4: __asm__ volatile ("csrw pmpaddr4,  %0" : : "r"(napot_addr)); break;
            case  5: __asm__ volatile ("csrw pmpaddr5,  %0" : : "r"(napot_addr)); break;
            case  6: __asm__ volatile ("csrw pmpaddr6,  %0" : : "r"(napot_addr)); break;
            case  7: __asm__ volatile ("csrw pmpaddr7,  %0" : : "r"(napot_addr)); break;
            case  8: __asm__ volatile ("csrw pmpaddr8,  %0" : : "r"(napot_addr)); break;
            case  9: __asm__ volatile ("csrw pmpaddr9,  %0" : : "r"(napot_addr)); break;
            case 10: __asm__ volatile ("csrw pmpaddr10, %0" : : "r"(napot_addr)); break;
            case 11: __asm__ volatile ("csrw pmpaddr11, %0" : : "r"(napot_addr)); break;
            case 12: __asm__ volatile ("csrw pmpaddr12, %0" : : "r"(napot_addr)); break;
            case 13: __asm__ volatile ("csrw pmpaddr13, %0" : : "r"(napot_addr)); break;
            case 14: __asm__ volatile ("csrw pmpaddr14, %0" : : "r"(napot_addr)); break;
            case 15: __asm__ volatile ("csrw pmpaddr15, %0" : : "r"(napot_addr)); break;
            default: return;
        }

        // 构造 pmpcfg 字节
        // A=3(NAPOT) | L=0(未锁定) | R/W/X 来自 ap 低 3 位
        uint8_t rwx = static_cast<uint8_t>(r.ap & 0x7u);
        if (r.execute_never) rwx &= ~0x4u;   // 清除 X 位
        uint8_t cfg = (3u << 3) | rwx;        // A=NAPOT
        _pmp_set_cfg(idx, cfg);

        __asm__ volatile ("fence.i" : : : "memory");
    }

    // RISC-V PMP 无需全局 enable/disable 操作：
    // 每个入口配置完成同时就即刻生效。
    // 提供函数体是为了将来屟结邀 mpu_enable()、不能是首次引入对现有代码的破坏性变更。
    inline void mpu_enable() noexcept {
        // 无操作: PMP 入口初始化即天就生效
    }

    inline void mpu_disable() noexcept {
        // 无操作: 不建议在运行时关闭 PMP 保护
    }
}

#endif // ARCH_IMPL_HPP
