#ifndef ARCH_IMPL_HPP
#define ARCH_IMPL_HPP

#include <stdint.h>
#include "board.h"

// =====================================================================
// ARMv7E-M (Cortex-M7) 架构实现
//
// 指令集特性: 完整 Thumb-2 + DSP/SIMD (ARMv7E-M: smlad/smuad 等)
// FPU: FPv5-SP (单精度, 32 个 S 寄存器), 在 early_init.cpp 中使能,
//      上下文切换由 cm7/context_switch.S 惰性保存 S16-S31
// Cache: 32-byte line 的 I/D-Cache, 在 early_init.cpp 冷启动使能
// 硬件周期计数: DWT CYCCNT
// 内存保护: PMSAv7, 最多 16 region (本实现沿用 8-region API, 索引兼容)
// =====================================================================

namespace Arch {
// ── 中断控制与状态寄存器 ──────────────────────────────────────
constexpr uintptr_t ICSR_ADDR = 0xE000ED04U;
constexpr uint32_t ICSR_PENDSVSET = (1UL << 28);

// ── 异常返回魔数 ─────────────────────────────────────────────
constexpr uint32_t EXC_RETURN_PSP = 0xFFFFFFFDU; // Thread mode + PSP, 无 FPU 帧
constexpr uint32_t XPSR_THUMB = 0x01000000U;

// ── SysTick 定时器 (ARMv7-M ARM B3.3) ────────────────────────
constexpr uintptr_t SYST_CSR_ADDR = 0xE000E010U;
constexpr uintptr_t SYST_RVR_ADDR = 0xE000E014U;
constexpr uintptr_t SYST_CVR_ADDR = 0xE000E018U;
constexpr uint32_t SYST_CSR_ENABLE = (1UL << 0);
constexpr uint32_t SYST_CSR_TICKINT = (1UL << 1);
constexpr uint32_t SYST_CSR_CLKSOURCE = (1UL << 2);

// ── DWT 周期计数器 ───────────────────────────────────────────
constexpr uintptr_t DEMCR_ADDR = 0xE000EDFCU;
constexpr uintptr_t DWT_CTRL_ADDR = 0xE0001000U;
constexpr uintptr_t DWT_CYCCNT_ADDR = 0xE0001004U;

// =====================================================================
// ── 最大系统调用中断优先级掩码 ───────────────────────────────
// 在 ARMv7E-M (Cortex-M7) 上，BASEPRI 寄存器用于屏蔽优先级数值大于等于该值的中断。
// 数值越小优先级越高。0x50 允许更高优先级（0x00 ~ 0x4F，如硬实时 Zero-Latency ISR）
// 在临界区内正常触发并被响应，显著降低关键中断延迟。
#ifndef CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY
#define CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY 0x50U
#endif

// =====================================================================
// 临界区 / 低功耗
// =====================================================================
inline void disable_interrupts() {
    __asm__ volatile("cpsid i" : : : "memory");
}

inline void enable_interrupts() {
    __asm__ volatile("cpsie i" : : : "memory");
}

inline uint32_t irq_save() {
    uint32_t prev_basepri;
    __asm__ volatile("mrs %0, basepri \n\t"
                     "msr basepri_max, %1 \n\t"
                     : "=r"(prev_basepri)
                     : "r"(static_cast<uint32_t>(CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY))
                     : "memory");
    return prev_basepri;
}

inline void irq_restore(uint32_t flags) {
    __asm__ volatile("msr basepri, %0 \n\t" : : "r"(flags) : "memory");
}

inline void wait_for_interrupt() {
    __asm__ volatile("dsb\n\t"
                     "wfi\n\t"
                     "isb\n\t"
                     :
                     :
                     : "memory");
}

// =====================================================================
// DWT 周期计数器 (Cortex-M7 硬件支持)
// =====================================================================
inline void init_dwt() {
    *reinterpret_cast<volatile uint32_t*>(DEMCR_ADDR) |= (1UL << 24);
    *reinterpret_cast<volatile uint32_t*>(DWT_CYCCNT_ADDR) = 0;
    *reinterpret_cast<volatile uint32_t*>(DWT_CTRL_ADDR) |= 1;
}

inline uint32_t get_cycle() {
    return *reinterpret_cast<volatile uint32_t*>(DWT_CYCCNT_ADDR);
}

inline uint32_t get_cycles_per_us() {
    return BOARD_SYSCLK_FREQ / 1000000;
}

// =====================================================================
// SysTick 系统节拍定时器
// =====================================================================
inline void systick_init(uint32_t hz) {
    volatile uint32_t* syst_csr = reinterpret_cast<volatile uint32_t*>(SYST_CSR_ADDR);
    volatile uint32_t* syst_rvr = reinterpret_cast<volatile uint32_t*>(SYST_RVR_ADDR);
    volatile uint32_t* syst_cvr = reinterpret_cast<volatile uint32_t*>(SYST_CVR_ADDR);

    *syst_csr = 0;
    *syst_rvr = (BOARD_SYSCLK_FREQ / hz) - 1;
    *syst_cvr = 0;
    *syst_csr = SYST_CSR_CLKSOURCE | SYST_CSR_TICKINT | SYST_CSR_ENABLE;

    init_dwt();
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
// Tickless Idle 唤醒定时器 (SysTick 重载借用方案, 与 cm3 相同)
// =====================================================================
inline uint32_t sleep_start_cycle = 0;

inline void start_wakeup_timer(uint32_t ticks) {
    volatile uint32_t* syst_rvr = reinterpret_cast<volatile uint32_t*>(SYST_RVR_ADDR);
    volatile uint32_t* syst_cvr = reinterpret_cast<volatile uint32_t*>(SYST_CVR_ADDR);

    sleep_start_cycle = get_cycle();

    uint32_t hz = 1000;
    uint32_t ticks_per_ms = BOARD_SYSCLK_FREQ / hz;
    uint32_t max_ticks = 0xFFFFFF / ticks_per_ms;

    if (ticks > max_ticks) {
        ticks = max_ticks;
    }

    *syst_rvr = (ticks * ticks_per_ms) - 1;
    *syst_cvr = 0;
}

inline uint32_t stop_wakeup_timer() {
    volatile uint32_t* syst_rvr = reinterpret_cast<volatile uint32_t*>(SYST_RVR_ADDR);
    uint32_t hz = 1000;
    uint32_t ticks_per_ms = BOARD_SYSCLK_FREQ / hz;

    *syst_rvr = ticks_per_ms - 1;

    uint32_t wake_cycle = get_cycle();
    uint32_t elapsed_cycles = wake_cycle - sleep_start_cycle;

    return elapsed_cycles / ticks_per_ms;
}

// =====================================================================
// 上下文切换触发
// =====================================================================
inline void trigger_context_switch() {
    *reinterpret_cast<volatile uint32_t*>(ICSR_ADDR) = ICSR_PENDSVSET;
}

// =====================================================================
// 线程初始栈帧伪造
//
// 与 cm4f 相同的 FPU 感知布局 (高地址 → 低地址):
//   xPSR | PC | LR(EXC_RETURN) | R12 | R3 | R2 | R1 | R0 | EXC_RETURN | R11..R4
//   [------ 硬件异常帧 (8 words) ------]  [--- 软件保存 (9 words) ---]
// 软件帧首字为 EXC_RETURN = 0xFFFFFFFD (无 FPU 扩展帧), 与
// cm7/context_switch.S 的 ldmia r0!, {r4-r11, lr} 恢复序列配套。
// =====================================================================
inline uint32_t* init_thread_stack(void (*task_entry)(void), uint32_t* stack_space, uint32_t stack_size) {
    uint32_t* top = stack_space + (stack_size / sizeof(uint32_t));

    // 1. 硬件异常帧 (8 words, 8-byte aligned)
    top--;
    *top = XPSR_THUMB; // xPSR: Thumb 位
    top--;
    *top = reinterpret_cast<uint32_t>(task_entry); // PC: 任务入口
    top--;
    *top = EXC_RETURN_PSP; // LR: Thread mode + PSP, 无 FPU 扩展帧
    top--;
    *top = 0x12121212; // R12
    top--;
    *top = 0x03030303; // R3
    top--;
    *top = 0x02020202; // R2
    top--;
    *top = 0x01010101; // R1
    top--;
    *top = 0x00000000; // R0: 任务参数占位

    // 2. 软件保存帧 (9 words) — 对应 context_switch.S 的 stmdb/ldmia {r4-r11, lr}
    top--;
    *top = EXC_RETURN_PSP; // EXC_RETURN (PendSV 首次恢复时读取, 识别 FPU 惰性状态)
    top--;
    *top = 0x11111111; // R11
    top--;
    *top = 0x10101010; // R10
    top--;
    *top = 0x09090909; // R9
    top--;
    *top = 0x08080808; // R8
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
// =====================================================================
[[noreturn]] inline void start_first_task(uint32_t* stack_ptr, void (*entry_point)(), uint32_t privilege = 0) {
    __asm__ volatile("ldm  %0!, {r4-r11, lr} \n\t" // 弹出 R4-R11 和 EXC_RETURN
                     "msr  psp, %0           \n\t"
                     "mov  r0, #2            \n\t" // CONTROL = 0b10: Thread mode + PSP (FPCA=0, FPU 惰性未激活)
                     "orr  r0, r0, %2        \n\t" // 合并特权级 (0: Kernel->2, 1: User->3)
                     "msr  control, r0      \n\t"
                     "isb                    \n\t"
                     "cpsie i                \n\t"
                     "bx   %1                \n\t"
                     :
                     : "r"(stack_ptr), "r"(reinterpret_cast<uint32_t>(entry_point)), "r"(privilege)
                     : "r0", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "lr", "memory");
    __builtin_unreachable();
}

// =====================================================================
// 动态修改当前特权级
// =====================================================================
inline void set_privilege(uint32_t privilege) {
    uint32_t control;
    __asm__ volatile("mrs %0, control" : "=r"(control));
    if (privilege) {
        control |= 1u; // Set nPRIV (1 = Unprivileged)
    } else {
        control &= ~1u; // Clear nPRIV (0 = Privileged)
    }
    __asm__ volatile("msr control, %0 \n\t"
                     "isb             \n\t"
                     :
                     : "r"(control)
                     : "memory");
}

// =====================================================================
// PMSAv7 内存保护单元 (Cortex-M7 支持 16 region, 沿用 8-region API)
// 寄存器模型: RNR 选区 / RBAR 地址 / RASR 属性
// =====================================================================
static constexpr uintptr_t MPU_CTRL = 0xE000ED94U;
static constexpr uintptr_t MPU_RNR = 0xE000ED98U;
static constexpr uintptr_t MPU_RBAR = 0xE000ED9CU;
static constexpr uintptr_t MPU_RASR = 0xE000EDA0U;

static constexpr uint32_t AP_PRIV_RW = 0b001;
static constexpr uint32_t AP_ALL_RW = 0b011;
static constexpr uint32_t AP_PRIV_RO = 0b101;
static constexpr uint32_t AP_ALL_RO = 0b110;

inline void mpu_configure_region(uint8_t idx, const MpuRegion& r) noexcept {
    auto* rnr = reinterpret_cast<volatile uint32_t*>(MPU_RNR);
    auto* rbar = reinterpret_cast<volatile uint32_t*>(MPU_RBAR);
    auto* rasr = reinterpret_cast<volatile uint32_t*>(MPU_RASR);

    *rnr = idx;
    *rbar = r.base & ~0x1Fu;

    uint32_t rasr_val = (1u << 0);                                              // ENABLE
    rasr_val |= ((static_cast<uint32_t>(r.size_pow2 - 1u) & 0x1Fu) << 1);       // SIZE
    rasr_val |= (static_cast<uint32_t>(r.subregion_disable_mask) & 0xFFu) << 8; // SRD (Sub-Region Disable)
    rasr_val |= (r.ap & 0x7u) << 24;                                            // AP
    if (r.is_device) {
        rasr_val |= (1u << 16); // B=1, C=0: Device
    } else {
        rasr_val |= (1u << 17) | (1u << 16); // B=1, C=1: Normal WB
    }
    if (r.execute_never) {
        rasr_val |= (1u << 28); // XN
    }
    *rasr = rasr_val;
    __asm__ volatile("dsb\n\t"
                     "isb\n\t"
                     :
                     :
                     : "memory");
}

inline void mpu_enable() noexcept {
    auto* ctrl = reinterpret_cast<volatile uint32_t*>(MPU_CTRL);
    auto* shcsr = reinterpret_cast<volatile uint32_t*>(0xE000ED24U);
    *ctrl = (1u << 2) | (1u << 0); // PRIVDEFENA | ENABLE
    *shcsr |= (1u << 16);          // MemManage enable
    __asm__ volatile("dsb\n\t"
                     "isb\n\t"
                     :
                     :
                     : "memory");
}

inline void mpu_disable() noexcept {
    auto* ctrl = reinterpret_cast<volatile uint32_t*>(MPU_CTRL);
    __asm__ volatile("dmb\n\t" : : : "memory");
    *ctrl = 0;
    __asm__ volatile("dsb\n\t"
                     "isb\n\t"
                     :
                     :
                     : "memory");
}

inline void switch_address_space(uintptr_t /*pgdir_base*/) noexcept {}
} // namespace Arch

#endif // ARCH_IMPL_HPP
