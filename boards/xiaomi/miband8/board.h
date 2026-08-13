#ifndef AURORA_BOARD_MIBAND8_H
#define AURORA_BOARD_MIBAND8_H

#include <stdint.h>

// ========================================================
// 1. 核心 SoC 配置 (Ambiq Apollo3 Blue)
// ========================================================
#define SOC_AMBIQ_APOLLO3_BLUE
#define CORE_CORTEX_M4F              // 启用 Cortex-M4F 架构
#define SYSTEM_CORE_CLOCK 96000000UL // 核心主频：96MHz

// ========================================================
// 2. 内存布局定义
// ========================================================
// SRAM: 总计 384KB
#define SRAM_BASE_ADDR 0x10000000
#define SRAM_SIZE (384 * 1024)

// Flash: 总计 1MB, 但 Bootloader 占用了前 448KB
#define FLASH_BASE_ADDR 0x00000000
#define FLASH_TOTAL_SIZE (1024 * 1024)
#define BOOTLOADER_SIZE (448 * 1024)
#define APP_FLASH_BASE_ADDR (FLASH_BASE_ADDR + BOOTLOADER_SIZE)
#define APP_FLASH_SIZE (FLASH_TOTAL_SIZE - BOOTLOADER_SIZE)

// ========================================================
// 3. 显示接口配置 (ST7789H2 AMOLED)
// ========================================================
#define DISPLAY_WIDTH 192
#define DISPLAY_HEIGHT 490
#define DISPLAY_SPI_PORT 0 // 假设使用 SPI0 控制器
#define PIN_DISP_CS 11     // 片选引脚 (示例)
#define PIN_DISP_DC 12     // 数据/命令控制引脚 (示例)
#define PIN_DISP_RST 13    // 硬件复位引脚 (示例)
#define PIN_DISP_BL 14     // 背光/亮度 PWM 控制引脚 (示例)

// ========================================================
// 4. 输入与传感器总线配置 (I2C)
// ========================================================
#define SENSOR_I2C_PORT 1 // 假设外设统一挂载在 I2C1

// 汇顶 GT316 单点触控 IC
#define I2C_ADDR_GT316 0x14
#define PIN_TOUCH_INT 15 // 触控硬件中断引脚

// GH3026 PPG 心率传感器
#define I2C_ADDR_GH3026 0x28

// BHI260AP 6轴加速度计
#define I2C_ADDR_BHI260AP 0x28 // 注意：实际硬件中需确认 I2C 地址是否冲突或通过引脚微调

// ========================================================
// 5. 无线与电源配置
// ========================================================
#define ENABLE_BLE_5_2 1         // 激活 BLE 5.2 协议栈编译
#define BATTERY_CAPACITY_MAH 190 // 电池容量设计值
#define PIN_BATTERY_ADC 31       // 电池电压检测 ADC 引脚 (示例)

// ========================================================
// 6. 板级初始化接口声明
// ========================================================
#ifdef __cplusplus
extern "C" {
#endif

// 初始化 MCU 时钟树、GPIO 复用及外设电源
void board_hardware_init(void);

// 让 CPU 进入低功耗 WFI 状态 (供 PowerManager 调用)
void board_enter_wfi(void);

#ifdef __cplusplus
}

namespace auroraos {
namespace hal {
class IGpioHal;
class ISpiHal;
class II2cHal;

IGpioHal* get_gpio_hal();
ISpiHal* get_spi_hal(int bus_id);
II2cHal* get_i2c_hal(int bus_id);
} // namespace hal
} // namespace auroraos
#endif

#endif // AURORA_BOARD_MIBAND8_H
