#ifndef BOARD_QEMU_AARCH64_VIRT_H
#define BOARD_QEMU_AARCH64_VIRT_H

// PL011 UART Base Address on QEMU AArch64 virt
#define BOARD_UART0_BASE 0x09000000
#define BOARD_SYSCLK_FREQ 24000000
#define BOARD_UART_BAUDRATE 115200

// GIC v2 Base Addresses on QEMU AArch64 virt
#define BOARD_GIC_DIST_BASE 0x08000000
#define BOARD_GIC_CPU_BASE  0x08010000

// Virtual display dimensions (used by UI framework compilation)
#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64

// Watchdog: QEMU virt software simulation
#define BOARD_WDT_HAS_SOFT 1

#endif // BOARD_QEMU_AARCH64_VIRT_H
