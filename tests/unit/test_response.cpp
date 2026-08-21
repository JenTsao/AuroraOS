// test_response.cpp — 自动响应系统单元测试
//
// 覆盖：
//   - 自动封禁：封禁/解封/超时/防火墙规则注入
//   - 隔离：任务挂起/恢复
//   - 取证快照：堆统计 + 内存区域副本 + 流量统计
//   - 响应引擎：按严重度执行封禁/快照策略

#include <gtest/gtest.h>
#include "../../security/response/response_engine.hpp"

using namespace aurora::response;

extern volatile uint32_t tick_count;

class ResponseTest : public ::testing::Test {
protected:
    void SetUp() override {
        tick_count = 1000;
        Scheduler::instance().init();
        static uint8_t heap[4096];
        KernelHeap::instance().init(heap, heap + sizeof(heap));
        ResponseEngine::instance().reset();
        ResponseEngine::instance().init();
    }
};

// ---- 自动封禁 ----

TEST_F(ResponseTest, AutoBlockBlocksAndUnblocks) {
    const uint32_t ip = 0xCB007107; // 203.0.113.7
    AutoBlockManager ab;

    EXPECT_TRUE(ab.block_ip(ip, 60000));
    EXPECT_TRUE(ab.is_blocked(ip));
    EXPECT_EQ(ab.get_block_count(), 1);

    EXPECT_TRUE(ab.unblock_ip(ip));
    EXPECT_FALSE(ab.is_blocked(ip));
    EXPECT_EQ(ab.get_block_count(), 0);
}

TEST_F(ResponseTest, AutoBlockExpires) {
    const uint32_t ip = 0xCB007108;
    AutoBlockManager ab;

    ab.block_ip(ip, 1000);
    EXPECT_TRUE(ab.is_blocked(ip));

    tick_count += 1001;
    ab.tick();
    EXPECT_FALSE(ab.is_blocked(ip));
}

TEST_F(ResponseTest, AutoBlockInjectsFirewallRule) {
    const uint32_t ip = 0xCB007109;
    AutoBlockManager ab;

    ab.block_ip(ip, 0); // 永久封禁

    // 防火墙规则表中应存在对应的 DROP 规则
    const FwRule* rules = FirewallEngine::instance().get_rule_table().get_rules();
    bool found = false;
    for (int i = 0; i < RuleTable::MAX_RULES; ++i) {
        if (rules[i].enabled && rules[i].match_src_ip && rules[i].src_ip == ip &&
            rules[i].action == FwAction::DROP) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);

    ab.reset(); // 清理注入的规则
}

// ---- 隔离 ----

TEST_F(ResponseTest, QuarantineSuspendsAndReleasesTask) {
    static uint32_t stack[64];
    TaskControlBlock* tcb = Scheduler::instance().create_task([]() {}, stack, sizeof(stack),
                                                              TaskPriority::Normal);
    ASSERT_NE(tcb, nullptr);
    const uint32_t tid = tcb->scheduler.id;

    QuarantineManager q;
    EXPECT_TRUE(q.quarantine_task(tid, 0)); // 永久隔离
    EXPECT_TRUE(q.is_quarantined(tid));
    EXPECT_EQ(tcb->scheduler.state, TaskState::Suspended);

    EXPECT_TRUE(q.release_task(tid));
    EXPECT_FALSE(q.is_quarantined(tid));
    EXPECT_EQ(tcb->scheduler.state, TaskState::Ready);
}

TEST_F(ResponseTest, QuarantineDeviceInjectsInterfaceRule) {
    QuarantineManager q;
    EXPECT_TRUE(q.quarantine_device("en0", 0));
    EXPECT_EQ(q.get_device_quarantine_count(), 1);

    EXPECT_TRUE(q.release_device("en0"));
    EXPECT_EQ(q.get_device_quarantine_count(), 0);
}

// ---- 取证快照 ----

TEST_F(ResponseTest, ForensicCaptureRecordsState) {
    uint8_t region[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

    ForensicRecorder fr;
    fr.set_traffic_stats(100, 5000);
    fr.capture(region, sizeof(region));

    EXPECT_EQ(fr.get_snapshot_count(), 1u);
    const ForensicRecorder::Snapshot* s = fr.get_snapshot(0);
    ASSERT_NE(s, nullptr);
    EXPECT_GT(s->heap_total, 0u);
    EXPECT_GT(s->heap_free, 0u);
    EXPECT_EQ(s->region_len, 16u);
    EXPECT_EQ(s->traffic_packets, 100u);
    EXPECT_EQ(s->traffic_bytes, 5000u);
    for (int i = 0; i < 16; ++i)
        EXPECT_EQ(s->region[i], region[i]);
}

// ---- 响应引擎 ----

TEST_F(ResponseTest, HandleCriticalAlertBlocksSource) {
    const uint32_t ip = 0xCB00710A;
    ResponseEngine& re = ResponseEngine::instance();
    const uint32_t actions_before = re.get_action_count();

    re.handle_alert(ResponseSeverity::Critical, ip, ResponseEngine::kNoTask, "test attack");

    EXPECT_TRUE(re.auto_block().is_blocked(ip));
    EXPECT_GT(re.get_action_count(), actions_before);
    EXPECT_GT(re.forensic().get_snapshot_count(), 0u);

    re.auto_block().unblock_ip(ip); // 清理
}

TEST_F(ResponseTest, HandleCriticalAlertQuarantinesTask) {
    static uint32_t stack[64];
    TaskControlBlock* tcb = Scheduler::instance().create_task([]() {}, stack, sizeof(stack),
                                                              TaskPriority::Normal);
    ASSERT_NE(tcb, nullptr);
    const uint32_t tid = tcb->scheduler.id;

    ResponseEngine& re = ResponseEngine::instance();
    re.handle_alert(ResponseSeverity::Critical, 0, tid, "rogue task");

    EXPECT_TRUE(re.quarantine().is_quarantined(tid));

    re.quarantine().release_task(tid); // 清理
}

TEST_F(ResponseTest, HandleMediumAlertSnapshots) {
    ResponseEngine& re = ResponseEngine::instance();
    const uint32_t snap_before = re.forensic().get_snapshot_count();

    re.handle_alert(ResponseSeverity::Medium, 0, ResponseEngine::kNoTask, "medium event");

    EXPECT_GT(re.forensic().get_snapshot_count(), snap_before);
    EXPECT_EQ(re.auto_block().get_block_count(), 0);
}

TEST_F(ResponseTest, HandleLowAlertIgnored) {
    ResponseEngine& re = ResponseEngine::instance();
    const uint32_t snap_before = re.forensic().get_snapshot_count();

    re.handle_alert(ResponseSeverity::Low, 0, ResponseEngine::kNoTask, "info");

    EXPECT_EQ(re.forensic().get_snapshot_count(), snap_before);
    EXPECT_EQ(re.auto_block().get_block_count(), 0);
}
