// board.cpp — Ambiq Apollo3 Blue 板级硬件初始化
// 目标：小米手环 8 (MiBand 8)

#include "board.h"
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
    // (a) SPI0 引脚复用为 IOM0 (SPI Master)：pad3=SCLK, pad4=nCE(CS), pad5=MOSI
    //     Apollo3 PADREG 每 pad 8 bit、4 pad 一个 32 位寄存器，基址 0x40010000。
    //     FUNCSEL=1 选择 IOM0 功能（MISO 未接，ST7789 只写不读）。
    volatile uint32_t* const padrega = reinterpret_cast<volatile uint32_t*>(0x40010000); // pads 0-3
    volatile uint32_t* const padregb = reinterpret_cast<volatile uint32_t*>(0x40010004); // pads 4-7
    *padrega = (*padrega & ~(0x7u << 24)) | (1u << 24); // pad3 → IOM0 SCLK
    *padregb = (*padregb & ~0x7u) | 1u;                 // pad4 → IOM0 nCE (CS)
    *padregb = (*padregb & ~(0x7u << 8)) | (1u << 8);   // pad5 → IOM0 MOSI

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
