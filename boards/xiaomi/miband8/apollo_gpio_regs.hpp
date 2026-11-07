#ifndef AURORA_BOARD_MIBAND8_APOLLO_GPIO_REGS_HPP
#define AURORA_BOARD_MIBAND8_APOLLO_GPIO_REGS_HPP

// ============================================================================
// Apollo3 Blue GPIO 寄存器映射与位域编码（纯计算，无 MMIO 访问）
//
// 抽成 header-only 的 constexpr 函数，是为了让 host 侧单元测试能够直接验证
// 地址计算与位域编码——板级 hal_impl.cpp 只在交叉编译的固件目标里编译，
// 无法在 PC 上实例化。
//
// 依据（2026-11-07 核实）：
//   * AmbiqSuite SDK utils/am_util_regdump.c 的 g_sRegdumpGPIO[] 寄存器表
//   * AmbiqSuite mcu/apollo3/hal/am_hal_gpio.h 的 am_hal_gpio_input_read /
//     am_hal_gpio_output_set 宏：RDA/WTSA + ((n & 0x20) >> 3)，位号 n & 0x1F
//   * Apollo3 Blue Datasheet v0.10.0 §11.7（Table 621 GPIO Register Map、
//     Table 623/649 PADREGA / CFGA 位域）
//   * Apollo3 Porting Guide 中 am_hal_gpio_pincfg_t 的位域映射注释
//
// ⚠️ 本表严格对应 **Apollo3 Blue**。Mi Band 8 的量产主控若为 Apollo4 Blue
//    Lite (AMA4BL)，上真机前必须按 Apollo4 datasheet 复核偏移与位域，见
//    DOCS/KNOWN_ISSUES.md「Apollo3 GPIO 位域与读分组缺陷」。
// ============================================================================

#include <stdint.h>

namespace auroraos {
namespace board {
namespace miband8 {
namespace apollo_gpio {

constexpr uint32_t kGpioBase = 0x40010000UL;
constexpr uint32_t kPinCount = 50u;

// ---- 寄存器偏移（相对 kGpioBase）----
constexpr uint32_t kPadregA = 0x000u; // PADREGA..PADREGM：4 pads/word, 8 bit/pad
constexpr uint32_t kCfgA = 0x040u;    // CFGA..CFGG：8 pads/word, 4 bit/pad
constexpr uint32_t kPadkey = 0x060u;  // 配置寄存器写保护钥匙
constexpr uint32_t kRda = 0x080u;     // 输入电平 GPIO 0..31
constexpr uint32_t kRdb = 0x084u;     // 输入电平 GPIO 32..49
constexpr uint32_t kWta = 0x088u;     // 输出数据 GPIO 0..31
constexpr uint32_t kWtb = 0x08Cu;     // 输出数据 GPIO 32..49
constexpr uint32_t kWtsa = 0x090u;    // 写后置位 GPIO 0..31
constexpr uint32_t kWtsb = 0x094u;    // 写后置位 GPIO 32..49
constexpr uint32_t kWtca = 0x098u;    // 写后清零 GPIO 0..31
constexpr uint32_t kWtcb = 0x09Cu;    // 写后清零 GPIO 32..49

constexpr uint32_t kPadkeyUnlock = 0x73u;
constexpr uint32_t kPadkeyLock = 0x00u;

// ---- PADREG：每 pad 8 bit ----
// bit0      PADnPULL    上拉使能（Apollo3 无通用下拉）
// bit1      PADnINPEN   输入使能；未置位时读回恒 0
// bit2      PADnSTRNG   驱动强度
// bit[5:3]  PADnFNCSEL  功能选择；0x3 = GPIO
// bit[7:6]  PADnRSEL    上拉阻值（仅 I2C 能力引脚有效）
constexpr uint32_t kPadFieldMask = 0xFFu;
constexpr uint32_t kPadPullShift = 0u;
constexpr uint32_t kPadInpenShift = 1u;
constexpr uint32_t kPadStrngShift = 2u;
constexpr uint32_t kPadFncselShift = 3u;
constexpr uint32_t kPadFncselGpio = 0x3u;
constexpr uint32_t kPadRselShift = 6u;

// ---- CFG：每 pad 4 bit ----
// bit0      GPIOxINCFG
// bit[2:1]  GPIOxOUTCFG 0=禁用 1=推挽 2=开漏 3=三态
// bit3      GPIOxINTD
constexpr uint32_t kCfgFieldMask = 0x0Fu;
constexpr uint32_t kCfgOutcfgShift = 1u;
constexpr uint32_t kCfgOutcfgPushpull = 0x1u;
constexpr uint32_t kCfgIntdShift = 3u;

constexpr uint32_t reg_address(uint32_t offset) noexcept {
    return kGpioBase + offset;
}

// PADREGA 管 0-3、PADREGB 管 4-7 ... PADREGM 管 48-49
constexpr uint32_t padreg_offset(uint32_t pin) noexcept {
    return kPadregA + (pin / 4u) * 4u;
}

constexpr uint32_t padreg_shift(uint32_t pin) noexcept {
    return (pin % 4u) * 8u;
}

// CFGA 管 0-7、CFGB 管 8-15 ... CFGG 管 48-49
constexpr uint32_t cfg_offset(uint32_t pin) noexcept {
    return kCfgA + (pin / 8u) * 4u;
}

constexpr uint32_t cfg_shift(uint32_t pin) noexcept {
    return (pin % 8u) * 4u;
}

// RDA/RDB、WTA/WTB、WTSA/WTSB、WTCA/WTCB 都按 bit5 分成两组：
// 低组 GPIO 0..31 用 A 寄存器，高组 GPIO 32..49 用紧随其后的 B 寄存器。
// 这与 SDK 的 `((n & 0x20) >> 3)` 地址增量完全一致（0x20>>3 == 4）。
constexpr uint32_t ab_offset(uint32_t pin, uint32_t reg_a) noexcept {
    return (pin < 32u) ? reg_a : (reg_a + 4u);
}

constexpr uint32_t ab_bit(uint32_t pin) noexcept {
    return 1u << (pin & 31u);
}

constexpr bool is_high_bank(uint32_t pin) noexcept {
    return pin >= 32u;
}

// 构造一个 pad 的 8bit 配置值。默认把功能选择设为 GPIO(0x3)。
constexpr uint32_t padreg_field(bool input_enable, bool pullup, uint32_t fncsel = kPadFncselGpio,
                                uint32_t rsel = 0u) noexcept {
    return ((fncsel & 0x7u) << kPadFncselShift) | ((rsel & 0x3u) << kPadRselShift) |
           (input_enable ? (1u << kPadInpenShift) : 0u) | (pullup ? (1u << kPadPullShift) : 0u);
}

// 构造一个 pad 的 4bit CFG 值。输入模式保持 0（INCFG=0、OUTCFG=禁用、INTD=0）。
// 注意不要在这里编码上下拉——旧实现把 (1u<<2)/(2u<<2) 写进 CFG，实际命中的
// 是 OUTCFG 与 INTD，而不是任何上下拉位。
constexpr uint32_t cfg_field(bool is_output) noexcept {
    return is_output ? (kCfgOutcfgPushpull << kCfgOutcfgShift) : 0u;
}

} // namespace apollo_gpio
} // namespace miband8
} // namespace board
} // namespace auroraos

#endif // AURORA_BOARD_MIBAND8_APOLLO_GPIO_REGS_HPP
