// board.cpp — Ambiq Apollo3 Blue 板级硬件初始化
// 目标：小米手环 8 (MiBand 8)

#include "board.h"

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

    // --- Step 2: GPIO 引脚复用初始化（最小集合） ---
    // Apollo3 的 GPIO 复用寄存器基址
    volatile uint32_t* const gpio_cfg = reinterpret_cast<volatile uint32_t*>(0x40010000);

    // SPI0 引脚：MOSI(0x05), SCLK(0x04), CS(0x03) → 连接 ST7789
    gpio_cfg[0] = (gpio_cfg[0] & ~0xFFu) | 0x03u; // 复用功能

    // I2C1 引脚：SDA(0x08), SCL(0x09) → 连接触控/心率/加速度计
    gpio_cfg[1] = (gpio_cfg[1] & ~0xFFu) | 0x04u;

    // GPIO 输出：背光 PWM
    volatile uint32_t* const gpio_out = reinterpret_cast<volatile uint32_t*>(0x40010080);
    *gpio_out |= (1u << PIN_DISP_BL);

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
