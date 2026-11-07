#ifndef STUB_BOARD_H
#define STUB_BOARD_H

// 空的板级配置桩，供主机端单元测试使用
// 覆盖原本位于 boards/xxx/board.h 的实现

#define BOARD_SYSCLK_FREQ 48000000

#define DISPLAY_WIDTH 192
#define DISPLAY_HEIGHT 490

// virtio-mmio 传输口（与 boards/qemu/rv32_virt/board.h 同值，仅供
// net/virtio_net.cpp 的 instance() 工厂编译；host 测试直接注入传输假件）
#define BOARD_VIRTIO_MMIO_BASE 0x10001000u
#define BOARD_VIRTIO_MMIO_STRIDE 0x1000u
#define BOARD_VIRTIO_MMIO_COUNT 8
#define BOARD_VIRTIO_NET_SLOT 0
#define BOARD_VIRTIO_BLK_SLOT 1
#define BOARD_VIRTIO_GPU_SLOT 2
#define BOARD_VIRTIO_GPU_SCALE 4

#endif // STUB_BOARD_H
