// board.cpp — Ambiq Apollo3 Blue 板级硬件初始化
// 目标：小米手环 8 (MiBand 8)

#include "board.h"
#include "apollo_gpio_regs.hpp"
#include "../../../hal/gpio_hal.hpp"

// ============================================================
// 板级硬件初始化：配置 96MHz 时钟树、GPIO、外设电源
// ============================================================
void board_hardware_init(void) {
    // --- Step 1: 使能 Apollo3 HFRC (96MHz 主时钟) ---
    // Apollo3 的时钟源选择：
    //   - HFRC (High-Frequency RC) = 96MHz 标称（上电默认）
    //   - 无需外部晶振，适合 MiBand 8 的低功耗场景
    volatile uint32_t* const pwrctrl = reinterpret_cast<volatile uint32_t*>(0x40004000);
    volatile uint32_t* const clkgen = reinterpret_cast<volatile uint32_t*>(0x40004004);

    // 确保 HFRC 上电且稳定（参考 Apollo3 Blue 数据手册 §5.2 Clock Generation）
    *pwrctrl |= (1u << 0); // HFRC power on
    for (volatile int i = 0; i < 1000; ++i) {
        __asm__ volatile("nop");
    }

    // 选择 HFRC 作为系统时钟源
    *clkgen = (*clkgen & ~0x7u) | 0x3u;

    // --- Step 2: 显示 SPI0 与控制引脚复用初始化 ---
    // (a) SPI0 引脚复用：Apollo3 写 PADREG/CFG 前必须先向 PADKEY(0x40010060) 写 0x73 解锁，
    //     且 FNCSEL 位域位于每 pad 的 bit[5:3]（右移 pad*8+3），不能写 bit[2:0]（那是
    //     PULL/INPEN/STRNG）。注：Apollo3 Blue 硬件 IOM0 固定位于 pad5=M0SCK(1),
    //     pad6=M0MISO(1), pad7=M0MOSI(1)，pad3/pad4 FNCSEL=2 为 NCE3/NCE4；此处沿用
    //     board.h 中的占位引脚号并在真机量产时按原理图核对 FNCSEL。
    namespace agpio = auroraos::board::miband8::apollo_gpio;
    volatile uint32_t* const padkey = reinterpret_cast<volatile uint32_t*>(agpio::reg_address(agpio::kPadkey));
    volatile uint32_t* const padrega =
        reinterpret_cast<volatile uint32_t*>(agpio::reg_address(agpio::kPadregA)); // pads 0-3
    volatile uint32_t* const padregb =
        reinterpret_cast<volatile uint32_t*>(agpio::reg_address(agpio::kPadregA + 4u)); // pads 4-7

    *padkey = agpio::kPadkeyUnlock;
    *padrega = agpio::update_padreg_fncsel(*padrega, PIN_DISP_CS, 1u);
    uint32_t regb = *padregb;
    regb = agpio::update_padreg_fncsel(regb, PIN_DISP_SCLK, 1u);
    regb = agpio::update_padreg_fncsel(regb, PIN_DISP_MOSI, 1u);
    *padregb = regb;
    *padkey = agpio::kPadkeyLock;

    // (b) 显示控制 GPIO 输出 (DC / RST / BL)，经 GPIO HAL 配置。
    //     背光先点亮、复位脚先拉高（脱离复位）；驱动 open() 时再走低-高复位脉冲。
    auroraos::hal::IGpioHal* const gpio = auroraos::hal::get_gpio_hal();
    gpio->init_pin(PIN_DISP_DC, auroraos::hal::GpioMode::Output, auroraos::hal::GpioPull::None);
    gpio->init_pin(PIN_DISP_RST, auroraos::hal::GpioMode::Output, auroraos::hal::GpioPull::None);
    gpio->init_pin(PIN_DISP_BL, auroraos::hal::GpioMode::Output, auroraos::hal::GpioPull::None);
    gpio->set_pin(PIN_DISP_RST, true); // 脱离复位
    gpio->set_pin(PIN_DISP_BL, true);  // 背光开

    // --- Step 3: 外设电源域上电（SPI, I2C, ADC） ---
    volatile uint32_t* const pwr_dev = reinterpret_cast<volatile uint32_t*>(0x40004040);
    *pwr_dev |= (1u << 0);  // SPI0 电源
    *pwr_dev |= (1u << 8);  // I2C1 电源
    *pwr_dev |= (1u << 16); // ADC 电源
}

// ============================================================
// 进入低功耗 WFI 状态（由 PowerManager 在 idle 任务中调用）
// ============================================================
void board_enter_wfi(void) {
    // DSB 确保未完成的总线事务刷入，再进入 WFI
    __asm__ volatile("dsb 0xF\n\t"
                     "wfi\n\t");
}

// ============================================================
// STIMER Compare A 唤醒中断处理（IRQ 23, CTIMER STMINTCLR @ 0x40008308）
// 必须清中断状态位，防止在 BASEPRI 放行后重复进 ISR
// ============================================================
extern "C" void STIMER_CMPR0_IRQHandler(void) {
    volatile uint32_t* const stmintclr = reinterpret_cast<volatile uint32_t*>(0x40008308UL);
    *stmintclr = 1u; // COMPAREA
}
