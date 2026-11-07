#ifndef BOARD_QEMU_RV32_VIRT_H
#define BOARD_QEMU_RV32_VIRT_H

// NS16550A UART Base Address (QEMU virt)
#define BOARD_UART0_BASE 0x10000000
#define BOARD_SYSCLK_FREQ 10000000
#define BOARD_UART_BAUDRATE 115200

// Core Local Interruptor (CLINT) Base Address
#define BOARD_CLINT_BASE 0x2000000

// Virtual display dimensions (no physical display; used by UI framework compilation)
#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64

// Watchdog: QEMU virt 无物理 WDT，使用软件模拟
#define BOARD_WDT_HAS_SOFT 1

// ---------------------------------------------------------------------------
// virtio-mmio 传输口（QEMU riscv virt：8 口，0x10001000 + i*0x1000）
// 槽号与 QEMU -device 声明顺序强耦合：本地/CI 命令必须按 net→blk→gpu 顺序
// 附加 -device virtio-*-device，否则槽号错位 probe 失败。
// ---------------------------------------------------------------------------
#define BOARD_VIRTIO_MMIO_BASE 0x10001000u
#define BOARD_VIRTIO_MMIO_STRIDE 0x1000u
#define BOARD_VIRTIO_MMIO_COUNT 8
#define BOARD_VIRTIO_MMIO_WINDOW (BOARD_VIRTIO_MMIO_STRIDE * BOARD_VIRTIO_MMIO_COUNT)
#define BOARD_VIRTIO_NET_SLOT 0
#define BOARD_VIRTIO_BLK_SLOT 1
#define BOARD_VIRTIO_GPU_SLOT 2
// 显示放大：逻辑 128x64 → 物理 512x256（B8G8R8X8，512KB 显存）
#define BOARD_VIRTIO_GPU_SCALE 4

#endif // BOARD_QEMU_RV32_VIRT_H
