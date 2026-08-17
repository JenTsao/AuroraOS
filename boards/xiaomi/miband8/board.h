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
// 3. 显示接口配置 (ST7789H2 AMOLED, 1.62" 192x490)
// ========================================================
#define DISPLAY_WIDTH 192
#define DISPLAY_HEIGHT 490
#define DISPLAY_SPI_PORT 0 // SPI0 (IOM0) 控制器

// SPI0 引脚 (Apollo3 复用，见 board.cpp 的 pad 复用配置)
#define PIN_DISP_MOSI 5 // SPI0 MOSI (主机数据出)
#define PIN_DISP_SCLK 4 // SPI0 SCLK (串行时钟)
#define PIN_DISP_CS 3   // SPI0 片选 (硬件 CS；若用软件 CS 需经 GPIO 控制)

// 显示控制 GPIO (数据/命令、硬件复位、背光)
// ⚠️ 引脚号需按 MiBand 8 实际原理图核对，当前为占位值。
#define PIN_DISP_DC 12  // 数据/命令控制 (DC/RS)
#define PIN_DISP_RST 13 // 硬件复位 (低有效)
#define PIN_DISP_BL 14  // 背光使能 (高有效；亮度由 WRDISBV 寄存器控制)

// 显示地址偏移：ST7789 有效显示区相对 GRAM 原点的列/行偏移，由面板规格书
// 决定；192x490 非标长条屏需量产标定，默认 0。
#define DISPLAY_X_OFFSET 0
#define DISPLAY_Y_OFFSET 0

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
// 5.1 Secure Element / OTP 密钥供应
// ========================================================
// Apollo3 Blue 的 customer OTP 区域用于烧录每设备唯一的 SoftBus 预共享
// 密钥 (32 字节 HMAC-SHA256)。生产时在工厂烧录；未烧录时该区域为全 0xFF，
// Secure Storage HAL 据此 fail-closed 拒绝返回密钥。
//
// 密钥记录布局 (OTP 偏移 0 起)：
//   [0..3]   魔术字 0x534F4654 ("SOFT")，标识密钥槽已烧录
//   [4..7]   密钥版本 uint32 (小端)
//   [8..39]  32 字节预共享密钥
#define OTP_CUSTOMER_BASE 0x0007F000UL // 例：Apollo3 customer OTP 起始地址
#define OTP_SOFTBUS_KEY_OFFSET 0x000UL  // SoftBus 密钥记录在该 OTP 区域的偏移
#define OTP_SOFTBUS_KEY_MAGIC 0x534F4654UL

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
class ISecureStorageHal;

IGpioHal* get_gpio_hal();
ISpiHal* get_spi_hal(int bus_id);
II2cHal* get_i2c_hal(int bus_id);
ISecureStorageHal* get_secure_storage_hal();
} // namespace hal
} // namespace auroraos
#endif

#endif // AURORA_BOARD_MIBAND8_H
