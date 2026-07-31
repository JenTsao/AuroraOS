// test_notification_center.cpp — 通知中心单元测试
//
// 测试策略 (cpp-testing):
//   - BleNotificationParser: 纯函数直接注入字节序列, 无任何硬件依赖
//   - PriorityNotificationQueue: 白盒测试堆排序正确性
//   - NotificationCenter: 通过 MockOverlay (INotificationOverlay 实现) 进行 DI
//   - GTest Fixtures 管理共用状态
//   - ASSERT_* 前置条件, EXPECT_* 多项校验

#include <gtest/gtest.h>
#include <gmock/gmock.h>

// 独立于硬件头: 在主机测试环境下定义 AURORA_HOST_TEST
// ui_config.hpp 将使用 DISPLAY_WIDTH/HEIGHT 宏而非 board.h
#ifndef AURORA_HOST_TEST
#define AURORA_HOST_TEST
#endif

#include "../../apps/notification_center.hpp"

using namespace aurora;
using ::testing::_;

// =============================================================
// MockOverlay — INotificationOverlay 的 GMock 实现 (用于 DI)
// =============================================================
class MockOverlay : public INotificationOverlay {
public:
    MOCK_METHOD(void, show,      (const Notification& n),   (override));
    MOCK_METHOD(void, hide,      (),                         (override));
    MOCK_METHOD(bool, is_visible, (),                        (const, noexcept, override));
    MOCK_METHOD(void, tick,      (uint32_t delta_ms),        (override));
};

// =============================================================
// 测试辅助：构建标准 TLV 报文
// =============================================================
static void append_tlv(uint8_t* buf, uint8_t& len,
                        uint8_t tag, const uint8_t* val, uint8_t val_len)
{
    buf[len++] = tag;
    buf[len++] = val_len;
    for (uint8_t i = 0; i < val_len; ++i) buf[len++] = val[i];
}

static Notification build_notification(uint32_t id, NotificationPriority p,
                                       NotificationCategory cat,
                                       const char* title, const char* body,
                                       uint32_t tick = 1000)
{
    uint8_t buf[128]{};
    uint8_t len = 0;

    const uint8_t id_bytes[4] = {
        static_cast<uint8_t>(id & 0xFF),
        static_cast<uint8_t>((id >> 8) & 0xFF),
        static_cast<uint8_t>((id >> 16) & 0xFF),
        static_cast<uint8_t>((id >> 24) & 0xFF)
    };
    append_tlv(buf, len, BleNotificationParser::kTagId, id_bytes, 4);

    const uint8_t prio_byte = static_cast<uint8_t>(p);
    append_tlv(buf, len, BleNotificationParser::kTagPriority, &prio_byte, 1);

    const uint8_t cat_byte = static_cast<uint8_t>(cat);
    append_tlv(buf, len, BleNotificationParser::kTagCategory, &cat_byte, 1);

    // Title
    uint8_t title_bytes[Notification::kTitleMaxLen]{};
    uint8_t tlen = 0;
    while (title[tlen] && tlen < Notification::kTitleMaxLen - 1u) {
        title_bytes[tlen] = static_cast<uint8_t>(title[tlen]); ++tlen;
    }
    append_tlv(buf, len, BleNotificationParser::kTagTitle, title_bytes, tlen);

    // Body
    uint8_t body_bytes[Notification::kBodyMaxLen]{};
    uint8_t blen = 0;
    while (body[blen] && blen < Notification::kBodyMaxLen - 1u) {
        body_bytes[blen] = static_cast<uint8_t>(body[blen]); ++blen;
    }
    append_tlv(buf, len, BleNotificationParser::kTagBody, body_bytes, blen);

    return BleNotificationParser::parse(buf, len, tick);
}

// =============================================================
// BleNotificationParser Tests
// =============================================================
class BleParserTest : public ::testing::Test {};

TEST_F(BleParserTest, ParsesAllTlvFieldsCorrectly) {
    const Notification n = build_notification(
        0xDEADBEEF,
        NotificationPriority::high,
        NotificationCategory::message,
        "Alice",
        "Hey, are you coming?",
        5000);

    EXPECT_EQ(n.id,       0xDEADBEEFu);
    EXPECT_EQ(n.priority, NotificationPriority::high);
    EXPECT_EQ(n.category, NotificationCategory::message);
    EXPECT_STREQ(n.title, "Alice");
    EXPECT_STREQ(n.body,  "Hey, are you coming?");
    EXPECT_EQ(n.timestamp, 5000u);
    EXPECT_FALSE(n.dismissed);
}

TEST_F(BleParserTest, EmptyBufferGivesDefaults) {
    const uint8_t empty[] = {};
    const Notification n = BleNotificationParser::parse(empty, 0, 0);
    EXPECT_EQ(n.id,       0u);
    EXPECT_EQ(n.priority, NotificationPriority::normal);
    EXPECT_EQ(n.category, NotificationCategory::app);
}

TEST_F(BleParserTest, TruncatedBufferDoesNotCrash) {
    // 报文只有 Tag+Len 头，没有 Value
    const uint8_t truncated[] = { BleNotificationParser::kTagId, 4 }; // value missing
    const Notification n = BleNotificationParser::parse(truncated, 2, 0);
    EXPECT_EQ(n.id, 0u); // 未能解析到 ID，应保持默认
}

TEST_F(BleParserTest, InvalidPriorityValueIsIgnored) {
    uint8_t buf[4] = {BleNotificationParser::kTagPriority, 1, 0xFF, 0};
    const Notification n = BleNotificationParser::parse(buf, 3, 0);
    // 0xFF > kMaxPriorityVal(3), 应忽略，保持默认 normal
    EXPECT_EQ(n.priority, NotificationPriority::normal);
}

TEST_F(BleParserTest, TitleTruncatedToMaxLen) {
    // 标题超出 15 字符后应截断
    uint8_t buf[128]{};
    uint8_t len = 0;
    const uint8_t long_title[20] = "ABCDEFGHIJKLMNOPQRST";
    append_tlv(buf, len, BleNotificationParser::kTagTitle, long_title, 20);
    const Notification n = BleNotificationParser::parse(buf, len, 0);
    // title 最多 kTitleMaxLen-1 = 15 个有效字符
    EXPECT_LE(n.title[Notification::kTitleMaxLen - 1], '\0');
}

TEST_F(BleParserTest, UnknownTagIsSkippedGracefully) {
    uint8_t buf[128]{};
    uint8_t len = 0;
    const uint8_t dummy[2] = {0xAA, 0xBB};
    append_tlv(buf, len, 0xFF, dummy, 2); // 未知 Tag
    const uint8_t prio = 3;
    append_tlv(buf, len, BleNotificationParser::kTagPriority, &prio, 1);
    const Notification n = BleNotificationParser::parse(buf, len, 0);
    // 未知 Tag 跳过，后续的 Priority TLV 应正常解析
    EXPECT_EQ(n.priority, NotificationPriority::critical);
}

// =============================================================
// PriorityNotificationQueue Tests
// =============================================================
class QueueTest : public ::testing::Test {
protected:
    PriorityNotificationQueue q;

    Notification make(NotificationPriority p, uint32_t ts = 0) {
        Notification n{};
        n.priority  = p;
        n.timestamp = ts;
        return n;
    }
};

TEST_F(QueueTest, EmptyQueuePeekReturnsNullptr) {
    EXPECT_EQ(q.peek(), nullptr);
    EXPECT_TRUE(q.empty());
}

TEST_F(QueueTest, PopFromEmptyQueueReturnsFalse) {
    Notification out;
    EXPECT_FALSE(q.pop(out));
}

TEST_F(QueueTest, SinglePushAndPop) {
    const Notification n = make(NotificationPriority::high, 42);
    ASSERT_TRUE(q.push(n));
    EXPECT_EQ(q.size(), 1);

    Notification out;
    ASSERT_TRUE(q.pop(out));
    EXPECT_EQ(out.priority,  NotificationPriority::high);
    EXPECT_EQ(out.timestamp, 42u);
    EXPECT_TRUE(q.empty());
}

TEST_F(QueueTest, PopAlwaysReturnsHighestPriority) {
    // 乱序插入不同优先级通知
    ASSERT_TRUE(q.push(make(NotificationPriority::low,      10)));
    ASSERT_TRUE(q.push(make(NotificationPriority::critical, 20)));
    ASSERT_TRUE(q.push(make(NotificationPriority::normal,   30)));
    ASSERT_TRUE(q.push(make(NotificationPriority::high,     40)));

    Notification out;
    ASSERT_TRUE(q.pop(out)); EXPECT_EQ(out.priority, NotificationPriority::critical);
    ASSERT_TRUE(q.pop(out)); EXPECT_EQ(out.priority, NotificationPriority::high);
    ASSERT_TRUE(q.pop(out)); EXPECT_EQ(out.priority, NotificationPriority::normal);
    ASSERT_TRUE(q.pop(out)); EXPECT_EQ(out.priority, NotificationPriority::low);
}

TEST_F(QueueTest, SamePriorityNewerTimestampWins) {
    ASSERT_TRUE(q.push(make(NotificationPriority::normal, 100)));
    ASSERT_TRUE(q.push(make(NotificationPriority::normal, 200)));
    ASSERT_TRUE(q.push(make(NotificationPriority::normal, 150)));

    Notification out;
    ASSERT_TRUE(q.pop(out));
    EXPECT_EQ(out.timestamp, 200u); // 最新的先出
}

TEST_F(QueueTest, FullQueueRejectsPush) {
    for (int i = 0; i < PriorityNotificationQueue::kCapacity; ++i) {
        ASSERT_TRUE(q.push(make(NotificationPriority::normal)));
    }
    EXPECT_EQ(q.size(), PriorityNotificationQueue::kCapacity);
    // 队列满时再 push 应返回 false
    EXPECT_FALSE(q.push(make(NotificationPriority::high)));
}

// =============================================================
// NotificationCenter Tests (with MockOverlay DI)
// =============================================================
class NotificationCenterTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_overlay_ = std::make_unique<MockOverlay>();
        // 初始状态：overlay 不在展示
        ON_CALL(*mock_overlay_, is_visible())
            .WillByDefault(::testing::Return(false));
        NotificationCenter::instance().set_overlay(mock_overlay_.get());
    }

    void TearDown() override {
        // 注销 overlay，避免跨测试用例污染
        NotificationCenter::instance().set_overlay(nullptr);
        NotificationCenter::instance().clear();
    }

    Notification make_notif(NotificationPriority p, uint32_t ts = 1000) {
        return build_notification(
            ts, p, NotificationCategory::message,
            "Test", "Body", ts);
    }

    std::unique_ptr<MockOverlay> mock_overlay_;
};

TEST_F(NotificationCenterTest, PostTriggersShowWhenOverlayIdle) {
    EXPECT_CALL(*mock_overlay_, show(_)).Times(1);

    const Notification n = make_notif(NotificationPriority::normal);
    EXPECT_TRUE(NotificationCenter::instance().post(n));
}

TEST_F(NotificationCenterTest, PostWhenVisibleQueuesButDoesNotShow) {
    // overlay 已在展示
    ON_CALL(*mock_overlay_, is_visible())
        .WillByDefault(::testing::Return(true));

    EXPECT_CALL(*mock_overlay_, show(_)).Times(0);

    const Notification n = make_notif(NotificationPriority::normal);
    NotificationCenter::instance().post(n);
    EXPECT_EQ(NotificationCenter::instance().pending_count(), 1);
}

TEST_F(NotificationCenterTest, DismissCurrentShowsNextFromQueue) {
    // 预填两条通知到队列（overlay 在展示中）
    ON_CALL(*mock_overlay_, is_visible())
        .WillByDefault(::testing::Return(true));
    NotificationCenter::instance().post(make_notif(NotificationPriority::high, 1000));
    NotificationCenter::instance().post(make_notif(NotificationPriority::normal, 2000));

    // dismiss 时先 hide，然后 show 下一条
    EXPECT_CALL(*mock_overlay_, hide()).Times(1);
    EXPECT_CALL(*mock_overlay_, show(_)).Times(1);
    NotificationCenter::instance().dismiss_current();
}

TEST_F(NotificationCenterTest, OnTickDrivesBannerAutoDismissAndDispatch) {
    // 模拟 overlay 状态转换: 初始为可见，tick() 调用后变为不可见
    bool visible = true;
    ON_CALL(*mock_overlay_, is_visible())
        .WillByDefault([&visible]() { return visible; });
    ON_CALL(*mock_overlay_, tick(3001u))
        .WillByDefault([&visible](uint32_t) { visible = false; });

    // 第一次 tick 时 overlay 是可见的（banner），入队时不应立即显示
    Notification queued = make_notif(NotificationPriority::low);
    NotificationCenter::instance().post(queued); // 入队

    EXPECT_CALL(*mock_overlay_, tick(3001u)).Times(1);
    EXPECT_CALL(*mock_overlay_, show(_)).Times(1); // 下一条自动弹出

    NotificationCenter::instance().on_tick(3001u);
}

TEST_F(NotificationCenterTest, FullQueueReturnsFalseOnExcess) {
    ON_CALL(*mock_overlay_, is_visible())
        .WillByDefault(::testing::Return(true));
    EXPECT_CALL(*mock_overlay_, show(_)).Times(0);

    // 填满队列
    for (int i = 0; i < PriorityNotificationQueue::kCapacity; ++i) {
        EXPECT_TRUE(NotificationCenter::instance().post(
            make_notif(NotificationPriority::normal)));
    }
    // 第 9 条应被拒绝
    EXPECT_FALSE(NotificationCenter::instance().post(
        make_notif(NotificationPriority::normal)));
}
