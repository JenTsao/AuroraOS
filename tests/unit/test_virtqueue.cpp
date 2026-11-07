// =============================================================================
// tests/unit/test_virtqueue.cpp
//
// split virtqueue 环管理（drivers/virtio/virtio_queue.*）主机测试
//
// 测试策略：
//  - 描述符链组装：头/中/尾 flags 与 next 链接、地址长度写入
//  - avail idx 推进、16 位回绕（>65536 次生产/消费循环）
//  - used ring pop 顺序与链释放（free_desc_count 回收一致性）
//  - 满环拒绝：空闲描述符不足 / n 非法 / 初始化参数非法
//  - notify() 经 IVirtioTransport 写 QueueNotify 寄存器
// =============================================================================

#include <gtest/gtest.h>
#include <cstdint>

#include "../../drivers/virtio/virtio_defs.hpp"
#include "../../drivers/virtio/virtio_queue.hpp"
#include "../stubs/virtio_fake_transport.hpp"

using auroraos::virtio::AvailRing;
using auroraos::virtio::BufSeg;
using auroraos::virtio::REG_QUEUE_NOTIFY;
using auroraos::virtio::UsedRing;
using auroraos::virtio::VIRTQ_MAX_QUEUE_SIZE;
using auroraos::virtio::VirtQueue;
using auroraos::virtio::VirtqDesc;
using auroraos::virtio::VirtqUsedElem;
using auroraos::virtio::VRING_DESC_F_NEXT;
using auroraos::virtio::VRING_DESC_F_WRITE;
using auroraos::virtio::testing::FakeTransport;

namespace {

struct QueueFixture {
    VirtqDesc desc[VIRTQ_MAX_QUEUE_SIZE]{};
    AvailRing avail{};
    UsedRing used{};
    FakeTransport transport;

    VirtQueue q;

    bool init(uint16_t size, uint16_t index = 0) {
        VirtQueue::Rings rings{desc, &avail, &used};
        return q.init(rings, size, index, transport);
    }

    // 模拟设备完成一个元素（写 used ring）
    void complete(uint16_t head, uint32_t len) {
        used.ring[used.idx % q.queue_size()] = VirtqUsedElem{head, len};
        used.idx = static_cast<uint16_t>(used.idx + 1);
    }
};

} // namespace

TEST(VirtQueueTest, InitRejectsBadArgs) {
    QueueFixture f;
    VirtQueue::Rings bad{nullptr, &f.avail, &f.used};
    EXPECT_FALSE(f.q.init(bad, 8, 0, f.transport));

    VirtQueue::Rings ok{f.desc, &f.avail, &f.used};
    EXPECT_FALSE(f.q.init(ok, 0, 0, f.transport));
    EXPECT_FALSE(f.q.init(ok, VIRTQ_MAX_QUEUE_SIZE + 1, 0, f.transport));

    ASSERT_TRUE(f.q.init(ok, 8, 3, f.transport));
    EXPECT_EQ(f.q.free_desc_count(), 8);
    EXPECT_EQ(f.q.queue_size(), 8);
}

TEST(VirtQueueTest, AddBufferChainsSegments) {
    QueueFixture f;
    ASSERT_TRUE(f.init(8));

    uint8_t buf0[16], buf1[32], buf2[8];
    BufSeg segs[3] = {
        {reinterpret_cast<uint64_t>(buf0), sizeof(buf0), false},
        {reinterpret_cast<uint64_t>(buf1), sizeof(buf1), true},
        {reinterpret_cast<uint64_t>(buf2), sizeof(buf2), true},
    };

    int head = f.q.add_buffer(segs, 3);
    ASSERT_EQ(head, 0);
    EXPECT_EQ(f.q.free_desc_count(), 5);

    // 头段：只读（device 不可写），带 NEXT
    EXPECT_EQ(f.desc[0].addr, reinterpret_cast<uint64_t>(buf0));
    EXPECT_EQ(f.desc[0].len, sizeof(buf0));
    EXPECT_EQ(f.desc[0].flags, VRING_DESC_F_NEXT);
    EXPECT_EQ(f.desc[0].next, 1);
    // 中段：可写 + NEXT
    EXPECT_EQ(f.desc[1].flags, static_cast<uint16_t>(VRING_DESC_F_NEXT | VRING_DESC_F_WRITE));
    EXPECT_EQ(f.desc[1].next, 2);
    // 尾段：可写，无 NEXT
    EXPECT_EQ(f.desc[2].flags, VRING_DESC_F_WRITE);
    EXPECT_EQ(f.desc[2].next, 0);

    // avail 推进
    EXPECT_EQ(f.avail.idx, 1);
    EXPECT_EQ(f.avail.ring[0], 0);
}

TEST(VirtQueueTest, AddBufferRejectsInvalidOrFull) {
    QueueFixture f;
    ASSERT_TRUE(f.init(4));

    uint8_t buf[8];
    BufSeg one{reinterpret_cast<uint64_t>(buf), sizeof(buf), false};
    // add_buffer(segs, n) 会读走 segs[0..n-1]，n=4 必须真有 4 段可读：
    // 之前只传 &one 却声明 n=4，等于越界读栈（ASan 报 stack-buffer-overflow）。
    BufSeg four[4] = {one, one, one, one};

    EXPECT_EQ(f.q.add_buffer(nullptr, 1), -1);
    EXPECT_EQ(f.q.add_buffer(&one, 0), -1);
    EXPECT_EQ(f.q.add_buffer(four, 5), -1); // 超过队列容量

    // 占满 4 个描述符
    ASSERT_EQ(f.q.add_buffer(four, 4), 0);
    EXPECT_EQ(f.q.free_desc_count(), 0);
    // 满环拒绝
    EXPECT_EQ(f.q.add_buffer(&one, 1), -1);
}

TEST(VirtQueueTest, PopUsedFreesChainInOrder) {
    QueueFixture f;
    ASSERT_TRUE(f.init(8));

    uint8_t buf[8];
    BufSeg seg_ro{reinterpret_cast<uint64_t>(buf), 4, false};
    BufSeg seg_rw{reinterpret_cast<uint64_t>(buf), 4, true};

    int head_a = f.q.add_buffer(&seg_ro, 1); // 单段链
    BufSeg chain2[2] = {seg_ro, seg_rw};
    int head_b = f.q.add_buffer(chain2, 2); // 两段链
    ASSERT_EQ(head_a, 0);
    ASSERT_EQ(head_b, 1);
    EXPECT_EQ(f.q.free_desc_count(), 5);

    // 设备乱序完成：先 b 后 a
    f.complete(static_cast<uint16_t>(head_b), 33);
    f.complete(static_cast<uint16_t>(head_a), 11);

    uint32_t len = 0;
    EXPECT_EQ(f.q.pop_used(&len), head_b);
    EXPECT_EQ(len, 33u);
    EXPECT_EQ(f.q.pop_used(&len), head_a);
    EXPECT_EQ(len, 11u);
    EXPECT_EQ(f.q.pop_used(&len), -1);

    // 两条链共 3 个描述符全部回收
    EXPECT_EQ(f.q.free_desc_count(), 8);
}

TEST(VirtQueueTest, NotifyWritesQueueNotifyRegister) {
    QueueFixture f;
    ASSERT_TRUE(f.init(8, 5));

    f.q.notify();
    EXPECT_EQ(f.transport.read32(REG_QUEUE_NOTIFY), 5u);
}

TEST(VirtQueueTest, AvailAndUsedIdxWrapAt16Bits) {
    QueueFixture f;
    ASSERT_TRUE(f.init(8));

    uint8_t buf[8];
    BufSeg seg{reinterpret_cast<uint64_t>(buf), sizeof(buf), false};

    // 70000 轮 add→complete→pop，跨过 uint16 回绕点
    const int kRounds = 70000;
    for (int i = 0; i < kRounds; ++i) {
        int head = f.q.add_buffer(&seg, 1);
        ASSERT_EQ(head, 0) << "round " << i;
        f.complete(0, 8);
        uint32_t len = 0;
        ASSERT_EQ(f.q.pop_used(&len), 0) << "round " << i;
    }
    EXPECT_EQ(f.avail.idx, static_cast<uint16_t>(kRounds));
    EXPECT_EQ(f.used.idx, static_cast<uint16_t>(kRounds));
    EXPECT_EQ(f.q.free_desc_count(), 8);
}

TEST(VirtQueueTest, OnInterruptDispatchesNotifyCallback) {
    QueueFixture f;
    ASSERT_TRUE(f.init(8));

    static int calls;
    calls = 0;
    f.q.set_notify_callback([](void* ctx, VirtQueue&) {
        ++*static_cast<int*>(ctx);
    },
                            &calls);
    f.q.on_interrupt();
    EXPECT_EQ(calls, 1);
}
