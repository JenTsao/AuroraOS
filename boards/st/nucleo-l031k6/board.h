#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

// ==========================================
// ST Nucleo-L031K6 板级定义
// MCU: STM32L031K6  (Cortex-M0+, ARMv6-M)
// Flash: 64KB @ 0x08000000
// RAM:   8KB  @ 0x20000000
// ==========================================

#define BOARD_MCU_STM32L031K6

// Cortex-M0+ — 无硬件 FPU，无 DWT 周期计数器
// PMSAv6-SC MPU 为可选（本板不启用）
#define BOARD_HAS_MPU 0
#define BOARD_HAS_FPU 0
#define BOARD_HAS_DWT 0

// 系统时钟：MSI 默认 2.1MHz，可倍频到 32MHz
// 这里假设 SystemClock_Config() 已将 MSI 切到 32MHz
#define BOARD_SYSCLK_FREQ  32000000U

// UART1: PA9(TX) PA10(RX) @ 115200
#define BOARD_UART0_BASE    0x40011400U   // USART1 基地址
#define BOARD_UART_BAUDRATE 115200U

// 内存布局（供 C 层常量使用，链接脚本有完整定义）
#define FLASH_SIZE          (64 * 1024)
#define RAM_SIZE            (8 * 1024)

#endif // BOARD_H
