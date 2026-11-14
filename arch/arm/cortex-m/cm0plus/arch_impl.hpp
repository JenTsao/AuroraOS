#ifndef ARCH_IMPL_HPP
#define ARCH_IMPL_HPP

#include <stdint.h>
#include "board.h"
#include "../common/tickless_math.hpp"

// 软件周期计数器 — 供 M0+ 使用（无 DWT CYCCNT）
// 定义在 interrupts.cpp（全局作用域）
extern volatile uint32_t g_sw_cycle_count;

// =====================================================================
// ARMv6-M (Cortex-M0+) 架构抽象层
//
// 与 M4 (ARMv7E-M) 的关键差异：
//   - 仅 Thumb 指令集（无 Thumb-2 32位指令）
//   - 只能访问低寄存器 r0-r7（STM/LDM 仅 r4-r7）
//   - 无 ITE 条件块、无 BFI 位域插入
//   - 无 ISB/DSB 屏障（DMB 可用）
//   - 无 DWT 周期计数器（用软件计数替代）
//   - CONTROL 寄存器仅 bit 0 (nPRIV) 可写
//   - MemManage/BusFault/UsageFault 全部合并到 HardFault
//   - MPU 为 PMSAv6-SC（本实现暂不启用）
// =====================================================================
namespace Arch {

// ── Cortex-M0+ 核心寄存器地址 ──────────────────────────────────
// NVIC/SCB 寄存器在 M0+ 上地址相同（ARMv6-M 兼容子集）
namespace tickless = auroraos::arch::cm_tickless;
static constexpr uintptr_t ICSR_ADDR = tickless::kIcsrAddr;
static constexpr uint32_t ICSR_PENDSVSET = tickless::kIcsrPendsvset;
static constexpr uint32_t ICSR_PENDSTCLR = tickless::kIcsrPendstclr;

// SysTick 寄存器（ARMv6-M 与 ARMv7-M 相同）
static constexpr uintptr_t SYST_CSR_ADDR = tickless::kSystCsrAddr;
static constexpr uintptr_t SYST_RVR_ADDR = tickless::kSystRvrAddr;
static constexpr uintptr_t SYST_CVR_ADDR = tickless::kSystCvrAddr;
static constexpr uint32_t SYST_CSR_ENABLE = tickless::kSystCsrEnable;
static constexpr uint32_t SYST_CSR_TICKINT = tickless::kSystCsrTickint;
static constexpr uint32_t SYST_CSR_CLKSOURCE = tickless::kSystCsrClksource;
static constexpr uint32_t SYST_CSR_COUNTFLAG = tickless::kSystCsrCountflag;

// EXC_RETURN 常量
static constexpr uint32_t EXC_RETURN_PSP = 0xFFFFFFFDU;
static constexpr uint32_t XPSR_THUMB = 0x01000000U;

// =====================================================================
// 底层内联汇编 — 中断控制
// M0+ 支持 CPSID I / CPSIE I（与 M4 相同）
// =====================================================================
inline void disable_interrupts() {
    __asm__ volatile("cpsid i" : : : "memory");
}

inline void enable_interrupts() {
    __asm__ volatile("cpsie i" : : : "memory");
}

inline uint32_t irq_save() {
    uint32_t flags;
    __asm__ volatile("mrs %0, primask \n\t"
                     "cpsid i         \n\t"
                     : "=r"(flags)
                     :
                     : "memory");
    return flags;
}

inline void irq_restore(uint32_t flags) {
    __asm__ volatile("msr primask, %0 \n\t" : : "r"(flags) : "memory");
}

inline void wait_for_interrupt() {
    __asm__ volatile("wfi" : : : "memory");
}

// =====================================================================
// 性能度量 — 软件周期计数器
// M0+ 没有 DWT CYCCNT，使用 SysTick 中断递增的软件计数器。
// 精度为 SysTick 周期（1ms @ 1000Hz），足以用于调度器度量。
// =====================================================================

inline uint32_t get_cycle() {
    return g_sw_cycle_count;
}

inline uint32_t get_cycles_per_us() {
    // 软件计数器以 tick 为单位，1 tick = 1ms = 1000us
    return 1000U;
}

// =====================================================================
// SysTick 初始化 — 与 M4 相同的寄存器，无 DWT 初始化
// =====================================================================
inline void systick_init(uint32_t hz) {
    volatile uint32_t* syst_csr = reinterpret_cast<volatile uint32_t*>(SYST_CSR_ADDR);
    volatile uint32_t* syst_rvr = reinterpret_cast<volatile uint32_t*>(SYST_RVR_ADDR);
    volatile uint32_t* syst_cvr = reinterpret_cast<volatile uint32_t*>(SYST_CVR_ADDR);

    *syst_csr = 0;                                                       // 1. 禁用
    *syst_rvr = (BOARD_SYSCLK_FREQ / hz) - 1;                            // 2. 重载值
    *syst_cvr = 0;                                                       // 3. 清零
    *syst_csr = SYST_CSR_CLKSOURCE | SYST_CSR_TICKINT | SYST_CSR_ENABLE; // 4. 启动
    // 注意：M0+ 无 DWT，跳过 init_dwt()
}

inline void disable_systick() {
    volatile uint32_t* syst_csr = reinterpret_cast<volatile uint32_t*>(SYST_CSR_ADDR);
    *syst_csr &= ~SYST_CSR_ENABLE;
}

inline void enable_systick() {
    volatile uint32_t* syst_csr = reinterpret_cast<volatile uint32_t*>(SYST_CSR_ADDR);
    *syst_csr |= SYST_CSR_ENABLE;
}

// =====================================================================
// Tickless Idle: 唤醒定时器
// M0+ 无 DWT，使用 SysTick 重载与 COUNTFLAG/CVR 计算经过的 tick 数
// =====================================================================
inline uint32_t sleep_reload_cycles = 0;

inline void start_wakeup_timer(uint32_t ticks) {
    volatile uint32_t* syst_csr = reinterpret_cast<volatile uint32_t*>(SYST_CSR_ADDR);
    volatile uint32_t* syst_rvr = reinterpret_cast<volatile uint32_t*>(SYST_RVR_ADDR);
    volatile uint32_t* syst_cvr = reinterpret_cast<volatile uint32_t*>(SYST_CVR_ADDR);

    constexpr uint32_t ticks_per_ms = BOARD_SYSCLK_FREQ / 1000U;
    constexpr uint32_t max_ticks = 0xFFFFFFU / ticks_per_ms;

    if (ticks == 0U) {
        ticks = 1U;
    } else if (ticks > max_ticks) {
        ticks = max_ticks;
    }

    sleep_reload_cycles = ticks * ticks_per_ms;
    *syst_rvr = sleep_reload_cycles - 1U;
    *syst_cvr = 0U; // 同时清 COUNTFLAG 与 CVR
    // disable_systick() 已清 ENABLE，这里必须重新置位 ENABLE 才能在 WFI 期间计数并唤醒
    *syst_csr |= SYST_CSR_ENABLE | SYST_CSR_TICKINT;
}

inline uint32_t stop_wakeup_timer() {
    volatile uint32_t* syst_csr = reinterpret_cast<volatile uint32_t*>(SYST_CSR_ADDR);
    volatile uint32_t* syst_rvr = reinterpret_cast<volatile uint32_t*>(SYST_RVR_ADDR);
    volatile uint32_t* syst_cvr = reinterpret_cast<volatile uint32_t*>(SYST_CVR_ADDR);
    volatile uint32_t* icsr = reinterpret_cast<volatile uint32_t*>(ICSR_ADDR);

    // 先读 CSR 快照 COUNTFLAG，再关闭 ENABLE 冻结 CVR
    const uint32_t csr = *syst_csr;
    *syst_csr = csr & ~SYST_CSR_ENABLE;
    const uint32_t cvr = *syst_cvr;

    if ((csr & SYST_CSR_COUNTFLAG) != 0U) {
        // 避免 PowerManager 补偿 sleep tick 后，irq_restore 时 SysTick_Handler 再重复计 1 tick
        *icsr = ICSR_PENDSTCLR;
    }

    constexpr uint32_t ticks_per_ms = BOARD_SYSCLK_FREQ / 1000U;
    // 恢复正常 1ms 心跳
    *syst_rvr = ticks_per_ms - 1U;
    *syst_cvr = 0U;

    return tickless::calc_systick_wakeup_elapsed_ticks(sleep_reload_cycles, csr, cvr, ticks_per_ms);
}

// =====================================================================
// 上下文切换触发 — 与 M4 相同（ICSR 寄存器地址相同）
// =====================================================================
inline void trigger_context_switch() {
    *reinterpret_cast<volatile uint32_t*>(ICSR_ADDR) = ICSR_PENDSVSET;
}

// =====================================================================
// 硬件线程初始栈帧伪造
//
// ARMv6-M 异常返回时硬件自动从 PSP 弹出 8 个 word:
//   r0, r1, r2, r3, r12, lr, pc, xPSR
//
// PendSV_Handler 手动保存/恢复 r4-r7（仅低寄存器）。
// 栈帧布局:
//   [top]  xPSR  (Thumb bit 必须置位)
//          PC    (任务入口)
//          LR    (EXC_RETURN)
//          r12   (占位，保持 8 word 对齐)
//          r3, r2, r1, r0  (硬件自动弹出)
//   ──── PendSV 软件保存边界 ────
//          r7, r6, r5, r4  (PendSV 手动弹出)
// =====================================================================
inline uint32_t* init_thread_stack(void (*task_entry)(void), uint32_t* stack_space, uint32_t stack_size) {
    uint32_t* top = stack_space + (stack_size / sizeof(uint32_t));

    // 硬件自动弹出的 8 个 word
    top--;
    *top = XPSR_THUMB; // xPSR
    top--;
    *top = reinterpret_cast<uint32_t>(task_entry); // PC
    top--;
    *top = EXC_RETURN_PSP; // LR (EXC_RETURN)
    top--;
    *top = 0x12121212; // R12 (占位)
    top--;
    *top = 0x03030303; // R3
    top--;
    *top = 0x02020202; // R2
    top--;
    *top = 0x01010101; // R1
    top--;
    *top = 0x00000000; // R0

    // PendSV 手动保存的 4 个低寄存器
    top--;
    *top = 0x07070707; // R7
    top--;
    *top = 0x06060606; // R6
    top--;
    *top = 0x05050505; // R5
    top--;
    *top = 0x04040404; // R4

    return top;
}

// =====================================================================
// 引导第一个任务
//
// M0+ 限制：
//   - 不能用 bfi 操作 CONTROL，改用 bics/orrs 手动位操作
//   - 不能用 isb（M0+ 无此指令），用 nop 替代
//   - 只能弹出 r4-r7（低寄存器）
// =====================================================================
[[noreturn]] __attribute__((naked)) inline void start_first_task(uint32_t* stack_ptr, void (*entry_point)(),
                                                                 uint32_t privilege = 0) {
    (void)stack_ptr;
    (void)entry_point;
    (void)privilege;
    __asm__ volatile(".syntax unified     \n\t"
                     "ldmia  r0!, {r4-r7}  \n\t" // 弹出 R4-R7 (r0 = stack_ptr from arg0)
                     "msr    psp, r0       \n\t" // 将更新后的指针写入 PSP
                     "movs   r0, r2        \n\t" // r0 = privilege (arg2)
                     "movs   r3, #1        \n\t"
                     "ands   r0, r3        \n\t" // r0 = privilege & 1
                     "msr    control, r0   \n\t" // 设置 CONTROL.nPRIV
                     ".syntax divided      \n\t"
                     "nop                  \n\t" // 等效 ISB
                     "cpsie  i             \n\t" // 全局开中断
                     "bx     r1            \n\t" // 跳入任务入口 (r1 = entry_point)
    );
    __builtin_unreachable();
}

// =====================================================================
// ARM PMSAv6-SC (Cortex-M0+) 内存保护单元实现
//
// PMSAv6-SC 与 PMSAv7 寄存器地址相同，但 RBAR 有 VALID 位 (bit4)
// 用于直接指定区域号，无需先写 RNR。
// 寄存器模型: RBAR (VALID|ADDR) / RASR (AP|SIZE|XN|B|C)
// =====================================================================
static constexpr uintptr_t MPU_CTRL = 0xE000ED94U;
static constexpr uintptr_t MPU_RNR = 0xE000ED98U;
static constexpr uintptr_t MPU_RBAR = 0xE000ED9CU;
static constexpr uintptr_t MPU_RASR = 0xE000EDA0U;

// AP 常量 (PMSAv6-SC Table B3-15, same as PMSAv7)
static constexpr uint32_t AP_PRIV_RW = 0b001;
static constexpr uint32_t AP_ALL_RW = 0b011;
static constexpr uint32_t AP_PRIV_RO = 0b101;
static constexpr uint32_t AP_ALL_RO = 0b110;

inline void mpu_configure_region(uint8_t idx, const MpuRegion& r) noexcept {
    volatile uint32_t* rnr = reinterpret_cast<volatile uint32_t*>(MPU_RNR);
    volatile uint32_t* rbar = reinterpret_cast<volatile uint32_t*>(MPU_RBAR);
    volatile uint32_t* rasr = reinterpret_cast<volatile uint32_t*>(MPU_RASR);

    *rnr = idx;
    // PMSAv6-SC: RBAR VALID bit (bit4) = 1 表示 REGION 字段有效
    *rbar = (r.base & ~0x1Fu) | (1u << 4);

    uint32_t rasr_val = (1u << 0);                                        // ENABLE
    rasr_val |= ((static_cast<uint32_t>(r.size_pow2 - 1u) & 0x1Fu) << 1); // SIZE
    rasr_val |= (static_cast<uint32_t>(r.subregion_disable_mask) & 0xFFu) << 8; // SRD (Sub-Region Disable)
    rasr_val |= (r.ap & 0x7u) << 24;                                      // AP
    if (r.is_device) {
        rasr_val |= (1u << 16); // B=1, C=0 : Device
    } else {
        rasr_val |= (1u << 17) | (1u << 16); // B=1, C=1 : Normal WB
    }
    if (r.execute_never) {
        rasr_val |= (1u << 28); // XN
    }
    *rasr = rasr_val;
    __asm__ volatile("dmb\n\t" : : : "memory"); // M0+ 有 DMB 但无 ISB
}

inline void mpu_enable() noexcept {
    volatile uint32_t* ctrl = reinterpret_cast<volatile uint32_t*>(MPU_CTRL);
    volatile uint32_t* shcsr = reinterpret_cast<volatile uint32_t*>(0xE000ED24U);
    *ctrl = (1u << 2) | (1u << 0); // PRIVDEFENA | ENABLE
    *shcsr |= (1u << 16);          // MemFaultEna
    __asm__ volatile("dmb\n\t" : : : "memory");
}

inline void mpu_disable() noexcept {
    volatile uint32_t* ctrl = reinterpret_cast<volatile uint32_t*>(MPU_CTRL);
    __asm__ volatile("dmb\n\t" : : : "memory");
    *ctrl = 0;
    __asm__ volatile("dmb\n\t" : : : "memory");
}

// =====================================================================
// 动态修改当前特权级
// =====================================================================
inline void set_privilege(uint32_t privilege) {
    uint32_t control;
    __asm__ volatile("mrs %0, control" : "=r"(control));
    if (privilege) {
        control |= 1u;
    } else {
        control &= ~1u;
    }
    __asm__ volatile("msr control, %0 \n\t"
                     "nop             \n\t"
                     :
                     : "r"(control)
                     : "memory");
}

inline void switch_address_space(uintptr_t /*pgdir_base*/) noexcept {}
} // namespace Arch

#endif // ARCH_IMPL_HPP
