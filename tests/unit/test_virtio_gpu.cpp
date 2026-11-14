// =============================================================================
// tests/unit/test_virtio_gpu.cpp
//
// virtio-gpu 显示后端（drivers/virtio/virtio_gpu.*）主机测试
//
// 测试策略：
//  - 纯函数：gpu_pixel_to_b8g8r8x8 阈值/解包、gpu_blit_scaled 放大块几何与
//    光标行主序推进（跨行回绕）
//  - 端到端：HookedTransport + VirtioGpuEmu 协作，验证 init 命令序列、
//    set_window→write_patch 面积累计触发一次收尾、窗口→TRANSFER rect/offset
//    映射、窗口外多余像素忽略
// =============================================================================

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <memory>

#include "../../drivers/virtio/virtio_gpu.hpp"
#include "../stubs/virtio_fake_transport.hpp"

using auroraos::virtio::GpuRect;
using auroraos::virtio::GPU_CMD_GET_DISPLAY_INFO;
using auroraos::virtio::GPU_CMD_RESOURCE_ATTACH_BACKING;
using auroraos::virtio::GPU_CMD_RESOURCE_CREATE_2D;
using auroraos::virtio::GPU_CMD_RESOURCE_FLUSH;
using auroraos::virtio::GPU_CMD_SET_SCANOUT;
using auroraos::virtio::GPU_CMD_TRANSFER_TO_HOST_2D;
using auroraos::virtio::DeviceId;
using auroraos::virtio::REG_DEVICE_ID;
using auroraos::virtio::REG_MAGIC_VALUE;
using auroraos::virtio::REG_QUEUE_NUM_MAX;
using auroraos::virtio::REG_VERSION;
using auroraos::virtio::VIRTIO_MMIO_MAGIC;
using auroraos::virtio::VirtioGpuDisplay;
using auroraos::virtio::gpu_blit_scaled;
using auroraos::virtio::gpu_pixel_to_b8g8r8x8;
using auroraos::virtio::testing::HookedTransport;
using auroraos::virtio::testing::VirtioGpuEmu;

namespace {

struct GpuFixture {
    HookedTransport transport;
    VirtioGpuEmu emu;
    // 显存 512KB + 环缓冲，堆上放置
    std::unique_ptr<VirtioGpuDisplay> gpu;

    // 逻辑 4×2、scale=2 → 物理 8×4，Mono1Bit
    GpuFixture() {
        transport.regs[REG_MAGIC_VALUE / 4] = VIRTIO_MMIO_MAGIC;
        transport.regs[REG_VERSION / 4] = 2;
        transport.regs[REG_DEVICE_ID / 4] = static_cast<uint32_t>(DeviceId::Gpu);
        transport.regs[REG_QUEUE_NUM_MAX / 4] = VirtioGpuDisplay::QUEUE_SIZE;
        transport.dev_feat_hi = 1; // VIRTIO_F_VERSION_1

        gpu = std::make_unique<VirtioGpuDisplay>(4, 2, 2, transport, true);
        emu.transport = &transport;
        transport.notify_hook = VirtioGpuEmu::on_notify_trampoline;
        transport.notify_ctx = &emu;
    }

    bool init() {
        // init() 会提交 GET_DISPLAY_INFO 等命令，模拟器必须先于 init 绑定
        const auto rings = gpu->ring_storage();
        emu.bind(rings.desc, rings.avail, rings.used, VirtioGpuDisplay::QUEUE_SIZE);
        return gpu->init();
    }
};

} // namespace

TEST(VirtioGpuTest, PixelConversionMonoThreshold) {
    EXPECT_EQ(gpu_pixel_to_b8g8r8x8(0x0000, true), 0x00000000u);
    EXPECT_EQ(gpu_pixel_to_b8g8r8x8(0x840F, true), 0x00000000u); // 阈值下
    EXPECT_EQ(gpu_pixel_to_b8g8r8x8(0x8410, true), 0x00FFFFFFu); // 阈值上
    EXPECT_EQ(gpu_pixel_to_b8g8r8x8(0xFFFF, true), 0x00FFFFFFu);
}

TEST(VirtioGpuTest, PixelConversionRgb565Unpack) {
    EXPECT_EQ(gpu_pixel_to_b8g8r8x8(0xF800, false), 0x00FF0000u); // 纯红
    EXPECT_EQ(gpu_pixel_to_b8g8r8x8(0x07E0, false), 0x0000FF00u); // 纯绿
    EXPECT_EQ(gpu_pixel_to_b8g8r8x8(0x001F, false), 0x000000FFu); // 纯蓝
    EXPECT_EQ(gpu_pixel_to_b8g8r8x8(0x0000, false), 0x00000000u);
    EXPECT_EQ(gpu_pixel_to_b8g8r8x8(0xFFFF, false), 0x00FFFFFFu);
}

TEST(VirtioGpuTest, BlitScaledMonoBlockGeometry) {
    // 2×2 逻辑、scale=2 → 物理 4×4
    uint32_t fb[4 * 4] = {};
    uint16_t src[2] = {0xFFFF, 0x0000};
    uint16_t cx = 0, cy = 0;

    uint32_t n = gpu_blit_scaled(reinterpret_cast<uint8_t*>(fb), 4, src, 2, 0, 0, 1, 2, true, cx, cy);
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(cx, 2);
    EXPECT_EQ(cy, 0);

    // 像素 0（白）占左 2×2 块，像素 1（黑）占右 2×2 块
    EXPECT_EQ(fb[0], 0x00FFFFFFu);
    EXPECT_EQ(fb[1], 0x00FFFFFFu);
    EXPECT_EQ(fb[2], 0x00000000u);
    EXPECT_EQ(fb[3], 0x00000000u);
    EXPECT_EQ(fb[4], 0x00FFFFFFu); // 第二行
    EXPECT_EQ(fb[7], 0x00000000u);
}

TEST(VirtioGpuTest, BlitCursorRowWrap) {
    // 窗口宽 2（win_x0=0, win_x1=1），喂 4 个像素 → 占两行
    uint32_t fb[4 * 4] = {};
    uint16_t src[4] = {0xFFFF, 0xFFFF, 0x0000, 0x0000};
    uint16_t cx = 0, cy = 0;

    uint32_t n = gpu_blit_scaled(reinterpret_cast<uint8_t*>(fb), 4, src, 4, 0, 0, 1, 1, true, cx, cy);
    EXPECT_EQ(n, 4u);
    EXPECT_EQ(cx, 2); // 末像素后游标推进到行末外
    EXPECT_EQ(cy, 1);

    // scale=1：像素 (0,0)(1,0) 白，(0,1)(1,1) 黑
    EXPECT_EQ(fb[0], 0x00FFFFFFu);
    EXPECT_EQ(fb[1], 0x00FFFFFFu);
    EXPECT_EQ(fb[4], 0x00000000u);
    EXPECT_EQ(fb[5], 0x00000000u);
}

TEST(VirtioGpuTest, InitCommandSequence) {
    GpuFixture f;
    ASSERT_TRUE(f.init());

    ASSERT_EQ(f.emu.cmd_count, 4u);
    EXPECT_EQ(f.emu.cmd_types[0], GPU_CMD_GET_DISPLAY_INFO);
    EXPECT_EQ(f.emu.cmd_types[1], GPU_CMD_RESOURCE_CREATE_2D);
    EXPECT_EQ(f.emu.cmd_types[2], GPU_CMD_RESOURCE_ATTACH_BACKING);
    EXPECT_EQ(f.emu.cmd_types[3], GPU_CMD_SET_SCANOUT);
    EXPECT_NE(f.emu.last_attach_addr, 0u);
    EXPECT_EQ(f.emu.last_attach_len, 8u * 4u * 4u);
    EXPECT_NE(f.emu.first_cmd_status & auroraos::virtio::STATUS_DRIVER_OK, 0u);
    EXPECT_EQ(f.emu.transfer_count, 0);
    EXPECT_EQ(f.emu.flush_count, 0);
}

TEST(VirtioGpuTest, InitProbeFailureIsFailSafe) {
    GpuFixture f;
    f.transport.regs[REG_MAGIC_VALUE / 4] = 0; // 无设备
    EXPECT_FALSE(f.init());
    EXPECT_FALSE(f.gpu->ready());
    // 未就绪时 write_patch 不崩溃不提交
    uint16_t px = 0xFFFF;
    f.gpu->set_window(0, 0, 0, 0);
    f.gpu->write_patch(&px, 1);
    EXPECT_EQ(f.emu.cmd_count, 0u);
}

TEST(VirtioGpuTest, AreaAccumulationTriggersSingleCommit) {
    GpuFixture f;
    ASSERT_TRUE(f.init());

    // 窗口 (1,0)-(2,1)：2×2=4 像素 → 物理 rect {2,0,4,4}，offset=(0*8+2)*4=8
    f.gpu->set_window(1, 0, 2, 1);

    uint16_t half[2] = {0xFFFF, 0x0000};
    f.gpu->write_patch(half, 2);
    EXPECT_EQ(f.emu.transfer_count, 0); // 面积未满不收尾

    f.gpu->write_patch(half, 2);
    EXPECT_EQ(f.emu.transfer_count, 1); // 恰好一次收尾
    EXPECT_EQ(f.emu.flush_count, 1);

    const GpuRect& r = f.emu.last_transfer_rect;
    EXPECT_EQ(r.x, 2u);
    EXPECT_EQ(r.y, 0u);
    EXPECT_EQ(r.width, 4u);
    EXPECT_EQ(r.height, 4u);
    EXPECT_EQ(f.emu.last_transfer_offset, 8u);
    EXPECT_EQ(f.gpu->last_commit_rect().width, 4u);
}

TEST(VirtioGpuTest, ExtraPixelsBeyondWindowIgnored) {
    GpuFixture f;
    ASSERT_TRUE(f.init());

    f.gpu->set_window(0, 0, 1, 1); // 4 像素窗口
    uint16_t five[5] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    f.gpu->write_patch(five, 5); // 第 5 个像素应被忽略

    EXPECT_EQ(f.emu.transfer_count, 1);
    EXPECT_EQ(f.emu.flush_count, 1);

    // 收尾后不再接受多余像素（窗口已失效）
    f.gpu->write_patch(five, 5);
    EXPECT_EQ(f.emu.transfer_count, 1);
}

TEST(VirtioGpuTest, RowByRowPatchAccumulates) {
    GpuFixture f;
    ASSERT_TRUE(f.init());

    // 模拟 FrameBuffer::flush 逐行发送形态：窗口 2×2，每次一行
    f.gpu->set_window(0, 0, 1, 1);
    uint16_t row[2] = {0xFFFF, 0x0000};
    f.gpu->write_patch(row, 2);
    EXPECT_EQ(f.emu.transfer_count, 0);
    f.gpu->write_patch(row, 2);
    EXPECT_EQ(f.emu.transfer_count, 1);
    EXPECT_EQ(f.emu.flush_count, 1);
}
