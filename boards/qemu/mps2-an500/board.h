#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

// ==========================================
// QEMU MPS2+ AN500 (Cortex-M7) 板级外设物理基地址
// 内存映射依据 QEMU hw/arm/mps2.c (FPGA_AN500):
//   ZBT SSRAM1   4MB @ 0x00000000 (代码区)
//   ZBT SSRAM2&3 4MB @ 0x20000000 (数据区)
//   SYSCLK = 25MHz / SysTick 参考时钟 1MHz
// 板载外设为 ARM CMSDK APB 外设族 (非 PL011)
// ==========================================
#define BOARD_UART0_BASE 0x40004000U
#define BOARD_SYSCLK_FREQ 25000000U
#define BOARD_UART_BAUDRATE 115200U

// AN500 无真实显示硬件; kernel.cpp 的全局显示/触控对象需要尺寸定义
// (与 rv32 virt 一致, 仅作为 FrameBuffer/OledDriver 模板参数)
#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64

#endif // BOARD_H
