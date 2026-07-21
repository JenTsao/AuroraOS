#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

// ==========================================
// TI LM3S6965-EVB 板级外设物理基地址
// 所有板级硬件常量集中于此，逻辑层只允许通过 BOARD_* 宏访问
// ==========================================
#define BOARD_UART0_BASE       0x4000C000U
#define BOARD_ETH_MAC_BASE     0x40048000U
#define BOARD_SYSCTL_BASE      0x400FE000U
#define BOARD_SYSCLK_FREQ      12000000U
#define BOARD_UART_BAUDRATE    115200U

// 板载 SSD1300 OLED 显示屏分辨率
#define DISPLAY_WIDTH          96
#define DISPLAY_HEIGHT         16

// 默认 MAC 地址 (QEMU 常用厂商前缀 52:54:00)
// 由网卡驱动在 init() 中写入硬件过滤寄存器，不再散落在业务逻辑中
#define BOARD_DEFAULT_MAC0     0x52U
#define BOARD_DEFAULT_MAC1     0x54U
#define BOARD_DEFAULT_MAC2     0x00U
#define BOARD_DEFAULT_MAC3     0x12U
#define BOARD_DEFAULT_MAC4     0x34U
#define BOARD_DEFAULT_MAC5     0x56U

#endif // BOARD_H
