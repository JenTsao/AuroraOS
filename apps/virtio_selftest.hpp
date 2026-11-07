// =============================================================================
// apps/virtio_selftest.hpp
//
// 板上 virtio 自检（shell `disktest` / `gputest` 命令）：输出机器可 grep 的
// PASS/FAIL 行，供 scripts/qemu_expectations 串口断言使用。
// =============================================================================
#ifndef AURORA_VIRTIO_SELFTEST_HPP
#define AURORA_VIRTIO_SELFTEST_HPP

// virtio-blk 写读校验（整扇区 + 部分扇区读-改-写）
void run_virtio_blk_selftest();

// virtio-gpu 测试图案扫描出（逐行 write_patch 全屏 + 收尾校验）
void run_virtio_gpu_selftest();

// 显示后端切换（shell `display oled|gpu` 命令）。
// use_gpu=true 且 gpu 已就绪时切到 virtio-gpu，否则保持/回退 mock OLED。
// 实现位于 apps/kernel.cpp（display_flush 分发状态的所有者）。
bool virtio_display_use_gpu(bool use_gpu);

#endif // AURORA_VIRTIO_SELFTEST_HPP
