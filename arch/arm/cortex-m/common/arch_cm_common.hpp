#ifndef AURORA_ARCH_CM_COMMON_HPP
#define AURORA_ARCH_CM_COMMON_HPP

// =====================================================================
// Cortex-M ARMv7-M 公共架构层（PMSAv7 MPU 家族）
//
// 本文件收录 cm3 / cm4 / cm7 三个变体中逐字节相同的实现：
//   - 中断与 BASEPRI 选择性掩蔽临界区
//   - DWT 周期计数器
//   - SysTick 系统节拍
//   - Tickless Idle 唤醒定时器
//   - PMSAv7 MPU 区域配置
//   - 特权级动态切换 / 上下文切换触发
//
// 各变体仅保留真实差异（见各目录 arch_impl.hpp）：
//   cm3     wait_for_interrupt 带 dsb/isb 序列；标准 8+8 词栈帧
//   cm4     wait_for_interrupt 为裸 wfi；栈帧与启动序列同 cm3
//   cm7     FPU 感知的 8+9 词哨兵值栈帧（PendSV 恢复序列含 lr）
//   cm4f    Apollo3：FPU + STIMER + 指针式常量风格，暂不并入本层
//   cm0plus 无 BASEPRI（PRIMASK 临界区）、无 DWT（软件周期计数），
//            SysTick CVR 地址亦不同，暂不并入本层
//
// 依赖约定：MpuRegion 由 kernel/core/arch_api.hpp 在包含本文件之前
// 定义，故本文件不得反向包含 arch_api.hpp（依赖方向：内核 → 架构）。
// =====================================================================

#include <stdint.h>
#include "board.h" // BOARD_SYSCLK_FREQ — 计算 SysTick 重装值与每微秒周期数

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
// 在 ARMv7-M 上，BASEPRI 寄存器用于屏蔽优先级数值大于等于该值的中断。
// 数值越小优先级越高。0x50（前 3/4 位有效）允许更高优先级（0x00 ~ 0x4F，如 BLE 硬实时/Zero-Latency ISR）
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

// =====================================================================
// DWT 周期计数器 (Cortex-M3/M4/M7 硬件支持)
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
// Tickless Idle 唤醒定时器
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
// PMSAv7 内存保护单元 (RNR/RBAR/RASR)
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

    uint32_t rasr_val = (1u << 0);                                        // ENABLE
    rasr_val |= ((static_cast<uint32_t>(r.size_pow2 - 1u) & 0x1Fu) << 1); // SIZE
    rasr_val |= (static_cast<uint32_t>(r.subregion_disable_mask) & 0xFFu) << 8; // SRD (Sub-Region Disable)
    rasr_val |= (r.ap & 0x7u) << 24;                                      // AP
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

#endif // AURORA_ARCH_CM_COMMON_HPP
