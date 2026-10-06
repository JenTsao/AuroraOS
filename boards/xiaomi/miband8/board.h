#ifndef AURORA_BOARD_MIBAND8_H
#define AURORA_BOARD_MIBAND8_H

#include <stddef.h>
#include <stdint.h>

// ========================================================
// 1. 核心 SoC 配置 (Ambiq Apollo3 Blue)
// ========================================================
#define SOC_AMBIQ_APOLLO3_BLUE
#define CORE_CORTEX_M4F              // 启用 Cortex-M4F 架构
#define SYSTEM_CORE_CLOCK 96000000UL // 核心主频：96MHz

// 硬件能力位（与 boards/st/nucleo-l031k6/board.h 的同族宏对齐）
//
// ⚠️ BOARD_HAS_DWT 必须在此【显式定义为 1】，不能用 `#if BOARD_HAS_DWT`
//    直接开条件编译。理由：该宏全仓库只有 nucleo-l031k6 定义过（值为 0），
//    其余板型均未定义；而未定义宏在 `#if` 中求值为 0，且 miband8 的构建带
//    `-w`（flags.make 实测）会吞掉 -Wundef 警告 → 埋点会被【静默编译掉】，
//    表现为「埋了但读到恒 0」，比不埋更难排查。
//
//    能力依据：Apollo3 Blue 为 Cortex-M4F，ARMv7E-M 架构带 DWT CYCCNT；
//    arch/arm/cortex-m/cm4f/arch_impl.hpp:74-76 的 init() 已配置
//    TRCENA/CYCCNTENA，故 get_cycle() 是真实 96MHz 级周期计数（非软件计数器）。
//
//    只声明 BOARD_HAS_DWT，不声明 BOARD_HAS_MPU/FPU —— grep 实测这两个宏在
//    miband8 构建中零引用，声明它们会给人「已有能力检测机制」的错觉。
#define BOARD_HAS_DWT 1

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

// 侧键 GPIO：Mi Band 8 右侧物理按键。无公开原理图，引脚号为占位值需量产标定
// （同 :62 DISPLAY_X_OFFSET 先例）。必须落在 GPIO 0-15：现网 hal_impl.cpp
// group_reg() 按 pin/16 分组，而真实寄存器为 RDA(0-31)/RDB(32-49)，
// GPIO 16-49 读数错误（见 DOCS/KNOWN_ISSUES.md「Apollo3 GPIO init_pin
// 位域编码与读分组缺陷」条目）。
#define PIN_BUTTON_SIDE 6
#define BUTTON_SIDE_ACTIVE_HIGH 0 // 低有效（按下拉低），配合内部上拉

// GH3026 PPG 心率传感器
//
// ⚠️ 本宏当前是【死宏】：全仓库零引用（grep 验证）。
//    HeartRateSensor（drivers/sensor/sensor_framework.hpp:52-84）目前是
//    【模拟桩】—— read() 直接返回 simulated_bpm_(默认 75)，从不触碰 IOM1，
//    因此并不会与下面的 BHI260AP 抢占 0x28 地址。
//
//    若将来给 HeartRateSensor 补上真实 I2C 读取，【必须先解决 0x28 冲突】：
//    同一总线上两个从机地址相同时，二者会同时 ACK，主控读回的是 SDA
//    wired-OR 的混合数据 → PPG 数据【100% 损坏】（非概率性、偶发）。
//    且损坏形态偏向 0x00 / 固定掩码（0 电平被强驱动），易定位但数据完全不可用。
#define I2C_ADDR_GH3026 0x28

// BHI260AP 6轴加速度计
//
// ⚠️ 地址确实与上面的 I2C_ADDR_GH3026 相同（0x28），且本地址是【活跃使用】的：
//    drivers/sensor/bhy2_driver.hpp:72-75 `BHY2_I2C_ADDR_DEFAULT 0x28`
//    → kBhy2I2cAddrDefault → watch_app.cpp:90 bhy2_.configure(i2c, 0x28)
//
//    当前【不构成实际故障】，因为 GH3026 侧是模拟桩、不发起 I2C 事务。
//    硬件若需双传感器共存，须通过板级 -D 覆盖本宏 / ADDR 引脚微调错开地址。
#define I2C_ADDR_BHI260AP 0x28

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

// 初始化 Apollo3 BLE HCI UART (UART1) 硬件与中断
void board_ble_uart_init(void);

// 板级 UART 中断/DMA 喂数接口
void board_ble_uart_feed_rx(uint8_t byte);
void board_ble_uart_feed_rx_bytes(const uint8_t* buf, size_t len);

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
