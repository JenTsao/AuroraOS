#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

// ==========================================
// ST Nucleo-L031K6 板级定义
// MCU: STM32L031K6  (Cortex-M0+, ARMv6-M)
// Flash: 64KB @ 0x08000000
// RAM:   8KB  @ 0x20000000
// ==========================================

// 防止重复定义（CMakeLists.txt 也通过 -D 传入）
#ifndef BOARD_MCU_STM32L031K6
#define BOARD_MCU_STM32L031K6
#endif

// Cortex-M0+ — 无硬件 FPU，无 DWT 周期计数器
// PMSAv6-SC MPU 已实现（8 区域，与 PMSAv7 寄存器地址相同）
#define BOARD_HAS_MPU 1
#define BOARD_HAS_FPU 0
#define BOARD_HAS_DWT 0

// 系统时钟：上电默认 MSI ~2.1MHz，uart_init() 中通过 HSI16 + PLL (x4 /2) 切到 32MHz
#define BOARD_SYSCLK_FREQ 32000000U

// ST-Link VCP 串口 (UM1956): USART2 @ 0x40004400 (PA2=TX, PA15=RX, AF4) @ 115200
#define BOARD_UART0_BASE 0x40004400U // USART2 基地址 (旧值 0x40011400 实为 TIM22)
#define BOARD_UART_BAUDRATE 115200U

// 显示配置（无真实显示屏，使用 128x128 虚拟 OLED 与 LM3S6965 一致）
#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 128

// 内存布局（供 C 层常量使用，链接脚本有完整定义）
#ifndef FLASH_SIZE
#define FLASH_SIZE (64 * 1024)
#endif
#ifndef RAM_SIZE
#define RAM_SIZE (8 * 1024)
#endif

#endif // BOARD_H
