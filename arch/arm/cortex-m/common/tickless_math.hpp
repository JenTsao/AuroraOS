#ifndef AURORA_ARCH_CM_COMMON_TICKLESS_MATH_HPP
#define AURORA_ARCH_CM_COMMON_TICKLESS_MATH_HPP

// ============================================================================
// Pure constexpr register constants and Tickless arithmetic helpers shared
// across Cortex-M0+ / Cortex-M4F arch backends and host unit tests (no ASM).
// ============================================================================

#include <stdint.h>

namespace auroraos {
namespace arch {
namespace cm_tickless {

// ---- Standard ARMv6-M / ARMv7-M SysTick & SCB ICSR ----
constexpr uintptr_t kIcsrAddr = 0xE000ED04U;
constexpr uint32_t kIcsrPendstclr = (1UL << 25);
constexpr uint32_t kIcsrPendsvset = (1UL << 28);

constexpr uintptr_t kSystCsrAddr = 0xE000E010U;
constexpr uintptr_t kSystRvrAddr = 0xE000E014U;
constexpr uintptr_t kSystCvrAddr = 0xE000E018U;
constexpr uint32_t kSystCsrEnable = (1UL << 0);
constexpr uint32_t kSystCsrTickint = (1UL << 1);
constexpr uint32_t kSystCsrClksource = (1UL << 2);
constexpr uint32_t kSystCsrCountflag = (1UL << 16);

// ---- Ambiq Apollo3 Blue STIMER (CTIMER 0x40008000 sub-block, apollo3.h) ----
constexpr uintptr_t kApollo3StimerStcfgAddr = 0x40008140U;
constexpr uintptr_t kApollo3StimerSttmrAddr = 0x40008144U;
constexpr uintptr_t kApollo3StimerScmpr0Addr = 0x40008150U;
constexpr uintptr_t kApollo3StimerStmintenAddr = 0x40008300U;
constexpr uintptr_t kApollo3StimerStmintstatAddr = 0x40008304U;
constexpr uintptr_t kApollo3StimerStmintclrAddr = 0x40008308U;

constexpr uint32_t kStimerClkselMask = 0x0000000FUL;
constexpr uint32_t kStimerClkselXtal32kHz = 0x00000003UL;
constexpr uint32_t kStimerCompareAEn = (1UL << 8);
constexpr uint32_t kStimerIntCompareA = (1UL << 0);
constexpr uint32_t kApollo3StimerCmpr0Irq = 23U;
constexpr uint32_t kApollo3StimerCmpr0IrqBit = (1UL << kApollo3StimerCmpr0Irq);

// Compute elapsed OS ticks from SysTick COUNTFLAG/CVR snapshot after WFI.
constexpr uint32_t calc_systick_wakeup_elapsed_ticks(uint32_t reload_cycles, uint32_t csr, uint32_t cvr,
                                                     uint32_t ticks_per_ms) noexcept {
    if (ticks_per_ms == 0U || reload_cycles == 0U) {
        return 0U;
    }
    if ((csr & kSystCsrCountflag) != 0U) {
        return reload_cycles / ticks_per_ms;
    }
    if (cvr == 0U || cvr > reload_cycles) {
        return 0U;
    }
    return (reload_cycles - cvr) / ticks_per_ms;
}

// Convert OS sleep ticks (1kHz) to Apollo3 SCMPR0 delta in 32.768kHz cycles.
// Per am_hal_stimer.c, delta must be >= 2 to avoid missing a compare edge.
constexpr uint32_t ticks_to_stimer_delta(uint32_t ticks_to_sleep) noexcept {
    const uint64_t cycles = (static_cast<uint64_t>(ticks_to_sleep) * 32768ULL) / 1000ULL;
    if (cycles < 2ULL) {
        return 2U;
    }
    if (cycles > 0x7FFFFFFFULL) {
        return 0x7FFFFFFFU;
    }
    return static_cast<uint32_t>(cycles);
}

// Convert wrapped 32-bit STTMR cycle delta back to elapsed OS ticks (1kHz).
constexpr uint32_t stimer_elapsed_to_ticks(uint32_t start_cycle, uint32_t wake_cycle) noexcept {
    const uint32_t elapsed_cycles =
        (wake_cycle >= start_cycle) ? (wake_cycle - start_cycle) : ((0xFFFFFFFFU - start_cycle) + wake_cycle + 1U);
    return static_cast<uint32_t>((static_cast<uint64_t>(elapsed_cycles) * 1000ULL) / 32768ULL);
}

} // namespace cm_tickless
} // namespace arch
} // namespace auroraos

#endif // AURORA_ARCH_CM_COMMON_TICKLESS_MATH_HPP
