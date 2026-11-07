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

// ---------------------------------------------------------------------------
// virtio-mmio 传输口（QEMU arm virt：32 口，0x0a000000 + i*0x200）
// 槽号与 QEMU -device 声明顺序强耦合：本地/CI 命令必须按 net→blk→gpu 顺序
// 附加 -device virtio-*-device，否则槽号错位 probe 失败。
// ---------------------------------------------------------------------------
#define BOARD_VIRTIO_MMIO_BASE 0x0a000000u
#define BOARD_VIRTIO_MMIO_STRIDE 0x200u
#define BOARD_VIRTIO_MMIO_COUNT 32
#define BOARD_VIRTIO_MMIO_WINDOW (BOARD_VIRTIO_MMIO_STRIDE * BOARD_VIRTIO_MMIO_COUNT)
#define BOARD_VIRTIO_NET_SLOT 0
#define BOARD_VIRTIO_BLK_SLOT 1
#define BOARD_VIRTIO_GPU_SLOT 2
// 显示放大：逻辑 128x64 → 物理 512x256（B8G8R8X8，512KB 显存）
#define BOARD_VIRTIO_GPU_SCALE 4

#endif // BOARD_QEMU_AARCH64_VIRT_H
