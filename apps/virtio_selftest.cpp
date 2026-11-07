// =============================================================================
// apps/virtio_selftest.cpp — 板上 virtio 自检实现
//
// 输出走 uart_puts，行格式机器可 grep（与 kernel_selftest 报告风格一致）：
//   [VIRTIO-BLK] write-read-verify: PASS|FAIL
//   [VIRTIO-GPU] scanout-<W>x<H>: PASS|FAIL
// =============================================================================
#include "virtio_selftest.hpp"

#include <stdint.h>
#include "board.h"

extern "C" void uart_puts(const char* s);

#ifdef CONFIG_VIRTIO_BLK
#include "../drivers/virtio/virtio_blk.hpp"
#endif
#ifdef CONFIG_VIRTIO_GPU
#include "../drivers/virtio/virtio_gpu.hpp"
#endif

namespace {

// 追加无符号十进制到缓冲（freestanding：不依赖 printf）
void append_uint(char*& p, uint32_t v) {
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    while (n > 0) {
        *p++ = tmp[--n];
    }
}

void append_str(char*& p, const char* s) {
    while (*s) {
        *p++ = *s++;
    }
    *p = '\0';
}

} // namespace

void run_virtio_blk_selftest() {
#ifdef CONFIG_VIRTIO_BLK
    using auroraos::virtio::VirtioBlk;
    VirtioBlk& blk = VirtioBlk::instance();
    if (!blk.ready()) {
        uart_puts("[VIRTIO-BLK] write-read-verify: FAIL (no device)\r\n");
        return;
    }

    static uint8_t wr[VirtioBlk::SECTOR_SIZE];
    static uint8_t rd[VirtioBlk::SECTOR_SIZE];
    for (uint32_t i = 0; i < sizeof(wr); ++i) {
        wr[i] = static_cast<uint8_t>(i * 13 + 7);
    }

    // 整扇区写读校验（扇区 7 作测试区）
    if (blk.write_blocks(7, 0, wr, sizeof(wr)) != 0 ||
        blk.read_blocks(7, 0, rd, sizeof(rd)) != 0) {
        uart_puts("[VIRTIO-BLK] write-read-verify: FAIL (io error)\r\n");
        return;
    }
    for (uint32_t i = 0; i < sizeof(wr); ++i) {
        if (rd[i] != wr[i]) {
            uart_puts("[VIRTIO-BLK] write-read-verify: FAIL (mismatch)\r\n");
            return;
        }
    }

    // 部分扇区读-改-写校验（偏移 100，长度 64）
    uint8_t patch[64];
    for (uint32_t i = 0; i < sizeof(patch); ++i) {
        patch[i] = static_cast<uint8_t>(0xA5 ^ i);
    }
    if (blk.write_blocks(7, 100, patch, sizeof(patch)) != 0 ||
        blk.read_blocks(7, 100, rd, sizeof(patch)) != 0) {
        uart_puts("[VIRTIO-BLK] write-read-verify: FAIL (io error)\r\n");
        return;
    }
    for (uint32_t i = 0; i < sizeof(patch); ++i) {
        if (rd[i] != patch[i]) {
            uart_puts("[VIRTIO-BLK] write-read-verify: FAIL (partial mismatch)\r\n");
            return;
        }
    }

    uart_puts("[VIRTIO-BLK] write-read-verify: PASS\r\n");
#else
    uart_puts("[VIRTIO-BLK] write-read-verify: FAIL (not built)\r\n");
#endif
}

void run_virtio_gpu_selftest() {
#ifdef CONFIG_VIRTIO_GPU
    using auroraos::virtio::VirtioGpuDisplay;
    VirtioGpuDisplay& gpu = VirtioGpuDisplay::instance();
    if (!gpu.ready()) {
        uart_puts("[VIRTIO-GPU] scanout: FAIL (no device)\r\n");
        return;
    }

    // 测试图案：逐行 write_patch 全屏黑白横条（覆盖 flush 的逐行形态），
    // 累计面积后自动 TRANSFER+FLUSH 收尾
    static uint16_t row[DISPLAY_WIDTH];
    gpu.set_window(0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1);
    for (uint32_t y = 0; y < DISPLAY_HEIGHT; ++y) {
        const uint16_t color = (y & 1) ? 0xFFFF : 0x0000;
        for (uint32_t x = 0; x < DISPLAY_WIDTH; ++x) {
            row[x] = color;
        }
        gpu.write_patch(row, DISPLAY_WIDTH);
    }

    char out[64];
    char* p = out;
    append_str(p, "[VIRTIO-GPU] scanout-");
    append_uint(p, DISPLAY_WIDTH * BOARD_VIRTIO_GPU_SCALE);
    append_str(p, "x");
    append_uint(p, DISPLAY_HEIGHT * BOARD_VIRTIO_GPU_SCALE);
    if (gpu.commit_errors() == 0) {
        append_str(p, ": PASS\r\n");
    } else {
        append_str(p, ": FAIL (submit error)\r\n");
    }
    uart_puts(out);
#else
    uart_puts("[VIRTIO-GPU] scanout: FAIL (not built)\r\n");
#endif
}
