// =============================================================================
// tests/unit/test_virtio_net.cpp
//
// virtio-net 驱动（net/virtio_net.*）主机测试
//
// 测试策略：
//  - init：probe/协商/双队列配置、MAC 从配置空间拷贝（VIRTIO_NET_F_MAC）
//  - send_frame：NetHdr+帧两段链提交、设备侧观察帧内容；非法长度拒绝
//  - receive_frame：设备投递帧→剥 NetHdr 拷出、空队列返回 0、max_len 截断、
//    取帧后补投缓冲（rx_posted 恢复）
//  - init 失败路径 fail-safe
// =============================================================================

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <memory>

#include "../../net/virtio_net.hpp"
#include "../stubs/virtio_fake_transport.hpp"

using auroraos::virtio::DeviceId;
using auroraos::virtio::NET_F_MAC_BIT;
using auroraos::virtio::REG_CONFIG_SPACE;
using auroraos::virtio::REG_DEVICE_ID;
using auroraos::virtio::REG_MAGIC_VALUE;
using auroraos::virtio::REG_QUEUE_NUM_MAX;
using auroraos::virtio::REG_VERSION;
using auroraos::virtio::VIRTIO_MMIO_MAGIC;
using auroraos::virtio::VirtioNet;
using auroraos::virtio::testing::HookedTransport;
using auroraos::virtio::testing::VirtioNetEmu;

namespace {

constexpr uint8_t EMU_MAC[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

struct NetFixture {
    HookedTransport transport;
    VirtioNetEmu emu;
    // 环缓冲 + RX 缓冲池约 40KB，堆上放置
    std::unique_ptr<VirtioNet> net;

    NetFixture() {
        transport.regs[REG_MAGIC_VALUE / 4] = VIRTIO_MMIO_MAGIC;
        transport.regs[REG_VERSION / 4] = 2;
        transport.regs[REG_DEVICE_ID / 4] = static_cast<uint32_t>(DeviceId::Net);
        transport.regs[REG_QUEUE_NUM_MAX / 4] = VirtioNet::QUEUE_SIZE;
        transport.dev_feat_hi = 1;              // VIRTIO_F_VERSION_1
        transport.dev_feat_lo = 1u << NET_F_MAC_BIT;

        // 配置空间 MAC（小端布局）
        transport.regs[REG_CONFIG_SPACE / 4] =
            EMU_MAC[0] | (EMU_MAC[1] << 8) | (EMU_MAC[2] << 16) | (EMU_MAC[3] << 24);
        transport.regs[REG_CONFIG_SPACE / 4 + 1] = EMU_MAC[4] | (EMU_MAC[5] << 8);

        net = std::make_unique<VirtioNet>(transport);
        transport.notify_hook = VirtioNetEmu::on_notify_trampoline;
        transport.notify_ctx = &emu;
    }

    bool init() {
        const bool ok = net->init();
        if (ok) {
            const auto& rx = net->rx_queue().rings();
            emu.bind_rx(rx.desc, rx.avail, rx.used, net->rx_queue().queue_size());
            const auto& tx = net->tx_queue().rings();
            emu.bind_tx(tx.desc, tx.avail, tx.used, net->tx_queue().queue_size());
        }
        return ok;
    }
};

} // namespace

TEST(VirtioNetTest, InitAndMacFromConfig) {
    EXPECT_EQ(sizeof(auroraos::virtio::NetHdr), 12u);
    NetFixture f;
    ASSERT_TRUE(f.init());
    EXPECT_TRUE(f.net->is_link_up());
    EXPECT_EQ(std::memcmp(f.net->get_mac(), EMU_MAC, 6), 0);
    // 全部 RX 缓冲已投递
    EXPECT_EQ(f.net->rx_posted(), VirtioNet::QUEUE_SIZE);
}

TEST(VirtioNetTest, InitFallsBackToDefaultMacWithoutFeature) {
    NetFixture f;
    f.transport.dev_feat_lo = 0; // 无 NET_F_MAC
    ASSERT_TRUE(f.init());
    const uint8_t expect[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    EXPECT_EQ(std::memcmp(f.net->get_mac(), expect, 6), 0);
}

TEST(VirtioNetTest, InitProbeFailureIsFailSafe) {
    NetFixture f;
    f.transport.regs[REG_VERSION / 4] = 1; // legacy 设备：明确拒绝
    EXPECT_FALSE(f.init());
    EXPECT_FALSE(f.net->is_link_up());
    uint8_t buf[64];
    EXPECT_EQ(f.net->receive_frame(buf, sizeof(buf)), 0);
    EXPECT_FALSE(f.net->send_frame(buf, 60));
}

TEST(VirtioNetTest, SendFrameDeliversPayload) {
    NetFixture f;
    ASSERT_TRUE(f.init());

    uint8_t frame[60];
    for (uint32_t i = 0; i < sizeof(frame); ++i) {
        frame[i] = static_cast<uint8_t>(i + 1);
    }
    ASSERT_TRUE(f.net->send_frame(frame, sizeof(frame)));
    EXPECT_EQ(f.emu.tx_count, 1);
    ASSERT_EQ(f.emu.last_tx_len, 60);
    EXPECT_EQ(std::memcmp(f.emu.last_tx_frame, frame, sizeof(frame)), 0);

    // 非法输入拒绝
    EXPECT_FALSE(f.net->send_frame(nullptr, 60));
    EXPECT_FALSE(f.net->send_frame(frame, 0));
    EXPECT_FALSE(f.net->send_frame(frame, VirtioNet::MAX_FRAME + 1));
}

TEST(VirtioNetTest, ReceiveFrameStripsHeaderAndReplenishes) {
    NetFixture f;
    ASSERT_TRUE(f.init());

    uint8_t frame[100];
    for (uint32_t i = 0; i < sizeof(frame); ++i) {
        frame[i] = static_cast<uint8_t>(0xA0 + i);
    }
    ASSERT_TRUE(f.emu.deliver_rx(frame, sizeof(frame)));

    uint8_t got[1514];
    std::memset(got, 0, sizeof(got));
    int n = f.net->receive_frame(got, sizeof(got));
    ASSERT_EQ(n, 100);
    EXPECT_EQ(std::memcmp(got, frame, 100), 0);
    // 取帧后缓冲已补投
    EXPECT_EQ(f.net->rx_posted(), VirtioNet::QUEUE_SIZE);

    // 空队列返回 0
    EXPECT_EQ(f.net->receive_frame(got, sizeof(got)), 0);
}

TEST(VirtioNetTest, ReceiveFrameTruncatesToMaxLen) {
    NetFixture f;
    ASSERT_TRUE(f.init());

    uint8_t frame[200];
    std::memset(frame, 0x5A, sizeof(frame));
    ASSERT_TRUE(f.emu.deliver_rx(frame, sizeof(frame)));

    uint8_t got[50];
    int n = f.net->receive_frame(got, sizeof(got));
    EXPECT_EQ(n, 50);
    EXPECT_EQ(got[0], 0x5A);
}

TEST(VirtioNetTest, MultipleFramesInOrder) {
    NetFixture f;
    ASSERT_TRUE(f.init());

    for (int round = 0; round < 3; ++round) {
        uint8_t frame[64];
        std::memset(frame, 0x30 + round, sizeof(frame));
        ASSERT_TRUE(f.emu.deliver_rx(frame, sizeof(frame)));

        uint8_t got[1514];
        int n = f.net->receive_frame(got, sizeof(got));
        ASSERT_EQ(n, 64) << "round " << round;
        EXPECT_EQ(got[0], static_cast<uint8_t>(0x30 + round));
    }
}
