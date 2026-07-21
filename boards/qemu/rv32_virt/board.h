#ifndef BOARD_QEMU_RV32_VIRT_H
#define BOARD_QEMU_RV32_VIRT_H

// NS16550A UART Base Address (QEMU virt)
#define BOARD_UART0_BASE 0x10000000
#define BOARD_SYSCLK_FREQ 10000000
#define BOARD_UART_BAUDRATE 115200

// Core Local Interruptor (CLINT) Base Address
#define BOARD_CLINT_BASE 0x2000000

// Virtual display dimensions (no physical display; used by UI framework compilation)
#define DISPLAY_WIDTH  128
#define DISPLAY_HEIGHT 64

#endif // BOARD_QEMU_RV32_VIRT_H
