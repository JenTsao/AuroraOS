// =============================================================================
// tests/unit/test_virtio_blk.cpp
//
// virtio-blk 驱动（drivers/virtio/virtio_blk.*）主机测试
//
// 测试策略：
//  - 纯函数：blk_build_chain 3 段链布局、blk_next_chunk 扇区/偏移/尾段换算
//  - 端到端：HookedTransport + VirtioBlkEmu（极简设备模拟器）协作完成请求，
//    覆盖整扇区直通、部分扇区读-改-写、边界拒绝、设备错误状态映射
//  - init 失败路径：probe 未命中 / 队列缺失 / 容量为 0 均 fail-safe
// =============================================================================

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <memory>

#include "../../drivers/virtio/virtio_blk.hpp"
#include "../stubs/virtio_fake_transport.hpp"

using auroraos::virtio::BLK_S_IOERR;
using auroraos::virtio::BLK_S_OK;
using auroraos::virtio::BLK_T_IN;
using auroraos::virtio::BLK_T_OUT;
using auroraos::virtio::BlkReqHeader;
using auroraos::virtio::BufSeg;
using auroraos::virtio::DeviceId;
using auroraos::virtio::REG_CONFIG_SPACE;
using auroraos::virtio::REG_DEVICE_ID;
using auroraos::virtio::REG_MAGIC_VALUE;
using auroraos::virtio::REG_QUEUE_NUM_MAX;
using auroraos::virtio::REG_VERSION;
using auroraos::virtio::VIRTIO_MMIO_MAGIC;
using auroraos::virtio::VirtioBlk;
using auroraos::virtio::VRING_DESC_F_NEXT;
using auroraos::virtio::VRING_DESC_F_WRITE;
using auroraos::virtio::blk_build_chain;
using auroraos::virtio::blk_next_chunk;
using auroraos::virtio::testing::HookedTransport;
using auroraos::virtio::testing::VirtioBlkEmu;

namespace {

constexpr uint32_t IMAGE_SECTORS = 4;
constexpr uint32_t IMAGE_BYTES = IMAGE_SECTORS * VirtioBlk::SECTOR_SIZE;

struct BlkFixture {
    HookedTransport transport;
    VirtioBlkEmu emu;
    uint8_t image[IMAGE_BYTES]{};

    // VirtioBlk 内含 4K 对齐的环缓冲（约 13KB），堆上放置避免撑爆测试栈
    std::unique_ptr<VirtioBlk> blk;

    BlkFixture() {
        transport.regs[REG_MAGIC_VALUE / 4] = VIRTIO_MMIO_MAGIC;
        transport.regs[REG_VERSION / 4] = 2;
        transport.regs[REG_DEVICE_ID / 4] = static_cast<uint32_t>(DeviceId::Block);
        transport.regs[REG_QUEUE_NUM_MAX / 4] = VirtioBlk::QUEUE_SIZE;
        transport.dev_feat_hi = 1; // VIRTIO_F_VERSION_1
        transport.regs[REG_CONFIG_SPACE / 4] = IMAGE_SECTORS; // capacity (le64 low)

        emu.image = image;
        emu.image_bytes = IMAGE_BYTES;

        blk = std::make_unique<VirtioBlk>("vda", transport);

        // 探测/协商/队列配置在 init() 内完成；绑定环缓冲需在 init 后。
        // emu 在首次 notify 前 bind 即可。
        transport.notify_hook = VirtioBlkEmu::on_notify_trampoline;
        transport.notify_ctx = &emu;
    }

    bool init() {
        const bool ok = blk->init();
        if (ok) {
            const auto& rings = blk->queue().rings();
            emu.bind(rings.desc, rings.avail, rings.used, blk->queue().queue_size());
        }
        return ok;
    }
};

} // namespace

TEST(VirtioBlkTest, BuildChainLayout) {
    BlkReqHeader hdr{};
    uint8_t data[512];
    uint8_t status = 0;
    BufSeg segs[3];

    blk_build_chain(&hdr, data, sizeof(data), true, &status, segs);
    EXPECT_EQ(segs[0].addr, reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(&hdr)));
    EXPECT_EQ(segs[0].len, sizeof(BlkReqHeader));
    EXPECT_FALSE(segs[0].device_writable);
    EXPECT_EQ(segs[1].addr, reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(data)));
    EXPECT_EQ(segs[1].len, sizeof(data));
    EXPECT_TRUE(segs[1].device_writable); // 读盘：设备写数据段
    EXPECT_EQ(segs[2].addr, reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(&status)));
    EXPECT_EQ(segs[2].len, 1u);
    EXPECT_TRUE(segs[2].device_writable);

    blk_build_chain(&hdr, data, sizeof(data), false, &status, segs);
    EXPECT_FALSE(segs[1].device_writable); // 写盘：设备只读数据段
}

TEST(VirtioBlkTest, NextChunkMapping) {
    uint32_t sector = 0, offset = 0, chunk = 0;

    // 整扇区对齐
    ASSERT_TRUE(blk_next_chunk(1024, 512, &sector, &offset, &chunk));
    EXPECT_EQ(sector, 2u);
    EXPECT_EQ(offset, 0u);
    EXPECT_EQ(chunk, 512u);

    // 扇区中段起始，尾段不跨扇区
    ASSERT_TRUE(blk_next_chunk(512 + 100, 200, &sector, &offset, &chunk));
    EXPECT_EQ(sector, 1u);
    EXPECT_EQ(offset, 100u);
    EXPECT_EQ(chunk, 200u);

    // 请求跨扇区时截断到当前扇区尾
    ASSERT_TRUE(blk_next_chunk(512 + 500, 100, &sector, &offset, &chunk));
    EXPECT_EQ(sector, 1u);
    EXPECT_EQ(offset, 500u);
    EXPECT_EQ(chunk, 12u);

    // 剩余 0 表示迭代结束
    EXPECT_FALSE(blk_next_chunk(0, 0, &sector, &offset, &chunk));
}

TEST(VirtioBlkTest, InitProbeFailureIsFailSafe) {
    BlkFixture f;
    f.transport.regs[REG_MAGIC_VALUE / 4] = 0xDEADBEEF; // 无设备
    EXPECT_FALSE(f.init());
    EXPECT_FALSE(f.blk->ready());
    // 未就绪时读写拒绝而不是崩溃
    uint8_t buf[16];
    EXPECT_EQ(f.blk->read_blocks(0, 0, buf, sizeof(buf)), -1);
}

TEST(VirtioBlkTest, InitRejectsMissingQueueAndZeroCapacity) {
    {
        BlkFixture f;
        f.transport.regs[REG_QUEUE_NUM_MAX / 4] = 0; // 队列不存在
        EXPECT_FALSE(f.init());
    }
    {
        BlkFixture f;
        f.transport.regs[REG_CONFIG_SPACE / 4] = 0; // 容量为 0
        EXPECT_FALSE(f.init());
    }
}

TEST(VirtioBlkTest, WriteReadRoundTripFullSector) {
    BlkFixture f;
    ASSERT_TRUE(f.init());
    EXPECT_EQ(f.blk->capacity_sectors(), IMAGE_SECTORS);

    uint8_t wbuf[VirtioBlk::SECTOR_SIZE];
    uint8_t rbuf[VirtioBlk::SECTOR_SIZE];
    for (uint32_t i = 0; i < sizeof(wbuf); ++i) {
        wbuf[i] = static_cast<uint8_t>(i * 7 + 1);
    }

    ASSERT_EQ(f.blk->write_blocks(2, 0, wbuf, sizeof(wbuf)), 0);
    std::memset(rbuf, 0, sizeof(rbuf));
    ASSERT_EQ(f.blk->read_blocks(2, 0, rbuf, sizeof(rbuf)), 0);
    EXPECT_EQ(std::memcmp(wbuf, rbuf, sizeof(wbuf)), 0);

    // 设备模拟器侧也应看到镜像被写入
    EXPECT_EQ(std::memcmp(f.image + 2 * VirtioBlk::SECTOR_SIZE, wbuf, sizeof(wbuf)), 0);
    EXPECT_EQ(f.emu.request_count, 2);
}

TEST(VirtioBlkTest, PartialSectorReadWriteUsesBounce) {
    BlkFixture f;
    ASSERT_TRUE(f.init());

    // 预置整扇区内容
    uint8_t full[VirtioBlk::SECTOR_SIZE];
    for (uint32_t i = 0; i < sizeof(full); ++i) {
        full[i] = static_cast<uint8_t>(i);
    }
    ASSERT_EQ(f.blk->write_blocks(1, 0, full, sizeof(full)), 0);

    // 部分写（扇区偏移 300，长度 20）：读-改-写不应破坏其余字节
    uint8_t patch[20];
    for (uint32_t i = 0; i < sizeof(patch); ++i) {
        patch[i] = 0xAA;
    }
    ASSERT_EQ(f.blk->write_blocks(1, 300, patch, sizeof(patch)), 0);

    uint8_t got[VirtioBlk::SECTOR_SIZE];
    ASSERT_EQ(f.blk->read_blocks(1, 0, got, sizeof(got)), 0);
    for (uint32_t i = 0; i < sizeof(got); ++i) {
        const uint8_t expect = (i >= 300 && i < 320) ? 0xAA : static_cast<uint8_t>(i);
        ASSERT_EQ(got[i], expect) << "byte " << i;
    }

    // 部分读（偏移 300，长度 20）
    uint8_t slice[20];
    ASSERT_EQ(f.blk->read_blocks(1, 300, slice, sizeof(slice)), 0);
    for (uint32_t i = 0; i < sizeof(slice); ++i) {
        EXPECT_EQ(slice[i], 0xAA);
    }
}

TEST(VirtioBlkTest, BoundsAndArgsRejected) {
    BlkFixture f;
    ASSERT_TRUE(f.init());

    uint8_t buf[64];
    EXPECT_EQ(f.blk->read_blocks(IMAGE_SECTORS, 0, buf, 4), -1);     // 越界块号
    EXPECT_EQ(f.blk->read_blocks(0, 500, buf, 20), -1);              // 跨块溢出
    EXPECT_EQ(f.blk->read_blocks(0, 513, buf, 4), -1);               // 偏移越界
    EXPECT_EQ(f.blk->read_blocks(0, 0, nullptr, 4), -1);             // 空指针
    EXPECT_EQ(f.blk->write_blocks(IMAGE_SECTORS, 0, buf, 4), -1);
    EXPECT_EQ(f.blk->write_blocks(0, 0, nullptr, 4), -1);
}

TEST(VirtioBlkTest, DeviceErrorStatusMapsToFailure) {
    BlkFixture f;
    ASSERT_TRUE(f.init());

    f.emu.forced_status = BLK_S_IOERR;
    uint8_t buf[VirtioBlk::SECTOR_SIZE];
    EXPECT_EQ(f.blk->read_blocks(0, 0, buf, sizeof(buf)), -1);
    EXPECT_EQ(f.blk->write_blocks(0, 0, buf, sizeof(buf)), -1);
}
