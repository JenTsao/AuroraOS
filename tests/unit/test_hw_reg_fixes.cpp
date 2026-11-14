#include <gtest/gtest.h>

#include "../../arch/arm/cortex-m/common/tickless_math.hpp"
#include "../../boards/st/nucleo-l031k6/board.h"

namespace tickless = auroraos::arch::cm_tickless;

TEST(CortexMTicklessRegs, SysTickCvrAddressIsWordAlignedAndNotLegacyMisaligned) {
    EXPECT_EQ(0xE000E010U, tickless::kSystCsrAddr);
    EXPECT_EQ(0xE000E014U, tickless::kSystRvrAddr);
    EXPECT_EQ(0xE000E018U, tickless::kSystCvrAddr);
    EXPECT_EQ(0U, tickless::kSystCvrAddr & 0x3U);
    EXPECT_NE(0xE000E015U, tickless::kSystCvrAddr);
}

TEST(CortexMTicklessRegs, SysTickWakeupElapsedHandlesCountflagEarlyWakeAndGuards) {
    constexpr uint32_t ticks_per_ms = BOARD_SYSCLK_FREQ / 1000U; // 32000 @ 32MHz
    constexpr uint32_t reload_10ms = 10U * ticks_per_ms;

    // 1. Timer expired (COUNTFLAG set) -> returns full sleep ticks regardless of CVR
    EXPECT_EQ(10U, tickless::calc_systick_wakeup_elapsed_ticks(reload_10ms,
                                                               tickless::kSystCsrEnable | tickless::kSystCsrCountflag,
                                                               ticks_per_ms * 9U, ticks_per_ms));

    // 2. Woke early after ~3.5 ms (6.5 ms remaining in CVR) -> returns 3 ms
    EXPECT_EQ(3U, tickless::calc_systick_wakeup_elapsed_ticks(
                      reload_10ms, tickless::kSystCsrEnable, reload_10ms - (ticks_per_ms * 3U + 16000U), ticks_per_ms));

    // 3. Invalid/unstarted zero CVR or out-of-range CVR -> safe 0
    EXPECT_EQ(0U, tickless::calc_systick_wakeup_elapsed_ticks(reload_10ms, tickless::kSystCsrEnable, 0U, ticks_per_ms));
    EXPECT_EQ(0U, tickless::calc_systick_wakeup_elapsed_ticks(reload_10ms, tickless::kSystCsrEnable, reload_10ms + 100U,
                                                              ticks_per_ms));
    EXPECT_EQ(0U, tickless::calc_systick_wakeup_elapsed_ticks(0U, tickless::kSystCsrCountflag, 0U, ticks_per_ms));
}

TEST(Apollo3StimerRegs, MatchesOfficialApollo3MemoryMapAndBitmasks) {
    EXPECT_EQ(0x40008140U, tickless::kApollo3StimerStcfgAddr);
    EXPECT_EQ(0x40008144U, tickless::kApollo3StimerSttmrAddr);
    EXPECT_EQ(0x40008150U, tickless::kApollo3StimerScmpr0Addr);
    EXPECT_EQ(0x40008300U, tickless::kApollo3StimerStmintenAddr);
    EXPECT_EQ(0x40008304U, tickless::kApollo3StimerStmintstatAddr);
    EXPECT_EQ(0x40008308U, tickless::kApollo3StimerStmintclrAddr);

    // COMPARE_A_EN is bit 8 (0x100), CLKSEL_XTAL_DIV1 is 0x3, IRQ is 23
    EXPECT_EQ(0x100U, tickless::kStimerCompareAEn);
    EXPECT_EQ(0x3U, tickless::kStimerClkselXtal32kHz);
    EXPECT_EQ(1U, tickless::kStimerIntCompareA);
    EXPECT_EQ(23U, tickless::kApollo3StimerCmpr0Irq);
}

TEST(Apollo3StimerRegs, DeltaConversionHandlesZeroLongSleepAndWrapAround) {
    // Delta < 2 must clamp to 2 (am_hal_stimer requirement)
    EXPECT_EQ(2U, tickless::ticks_to_stimer_delta(0U));
    // 1000 ticks (1s) == 32768 cycles
    EXPECT_EQ(32768U, tickless::ticks_to_stimer_delta(1000U));
    // >131s sleep must not overflow 32-bit multiplication before dividing by 1000
    EXPECT_EQ((200000ULL * 32768ULL) / 1000ULL, tickless::ticks_to_stimer_delta(200000U));
    // Extremely large request clamps to 0x7FFFFFFF
    EXPECT_EQ(0x7FFFFFFFU, tickless::ticks_to_stimer_delta(0xFFFFFFFFU));

    // Normal & wrapped 32-bit counter delta -> ticks
    EXPECT_EQ(1000U, tickless::stimer_elapsed_to_ticks(10000U, 10000U + 32768U));
    EXPECT_EQ(1000U, tickless::stimer_elapsed_to_ticks(0xFFFFFF00U, 32768U - 256U));
}

TEST(NucleoL031K6BoardConfig, UartBasePointsToUsart2VcpNotTim22) {
    EXPECT_EQ(0x40004400U, BOARD_UART0_BASE);
    EXPECT_NE(0x40011400U, BOARD_UART0_BASE);
    EXPECT_EQ(32000000U, BOARD_SYSCLK_FREQ);
    EXPECT_EQ(115200U, BOARD_UART_BAUDRATE);
}
