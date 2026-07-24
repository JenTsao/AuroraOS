#include <gtest/gtest.h>
#include <cstring>
#include "../../vfs/procfs.hpp"
#include "../../kernel/ipc.hpp"

using namespace auroraos::kernel;

// ==========================================
// UptimeNode Tests
// ==========================================
TEST(ProcFsTest, UptimeNodeShowsUptime) {
    extern volatile uint32_t tick_count;
    tick_count = 90412; // 1 min 30 sec 412 ms

    UptimeNode node;
    char buf[128] = {0};
    int n = node.read(buf, sizeof(buf), 0, nullptr);

    EXPECT_GT(n, 0);
    EXPECT_STREQ(buf, "uptime 0:01:30.412\n");
}

TEST(ProcFsTest, UptimeNodeZeroTicks) {
    extern volatile uint32_t tick_count;
    tick_count = 0;

    UptimeNode node;
    char buf[128] = {0};
    int n = node.read(buf, sizeof(buf), 0, nullptr);

    EXPECT_GT(n, 0);
    EXPECT_STREQ(buf, "uptime 0:00:00.000\n");
}

TEST(ProcFsTest, UptimeNodeOffsetReturnsZero) {
    UptimeNode node;
    char buf[128] = {0};
    int n = node.read(buf, sizeof(buf), 1, nullptr);
    EXPECT_EQ(n, 0);
}

TEST(ProcFsTest, UptimeNodeLargeUptime) {
    extern volatile uint32_t tick_count;
    tick_count = 3661000; // 1h 1m 1s

    UptimeNode node;
    char buf[128] = {0};
    int n = node.read(buf, sizeof(buf), 0, nullptr);

    EXPECT_GT(n, 0);
    EXPECT_STREQ(buf, "uptime 1:01:01.000\n");
}

// ==========================================
// IrqNode Tests
// ==========================================
TEST(ProcFsTest, IrqNodeShowsHeader) {
    IrqNode node;
    char buf[256] = {0};
    int n = node.read(buf, sizeof(buf), 0, nullptr);

    EXPECT_GT(n, 0);
    // Should contain key metric labels
    EXPECT_NE(strstr(buf, "irq_latency"), (char*)nullptr);
    EXPECT_NE(strstr(buf, "ctx_switch"), (char*)nullptr);
    EXPECT_NE(strstr(buf, "heap_alloc"), (char*)nullptr);
}

TEST(ProcFsTest, IrqNodeOffsetReturnsZero) {
    IrqNode node;
    char buf[256] = {0};
    int n = node.read(buf, sizeof(buf), 1, nullptr);
    EXPECT_EQ(n, 0);
}

// ==========================================
// CapsNode Tests
// ==========================================
TEST(ProcFsTest, CapsNodeShowsHeader) {
    Scheduler::instance().init();

    CapsNode node;
    char buf[512] = {0};
    int n = node.read(buf, sizeof(buf), 0, nullptr);

    EXPECT_GT(n, 0);
    EXPECT_NE(strstr(buf, "TID"), (char*)nullptr);
    EXPECT_NE(strstr(buf, "Slot"), (char*)nullptr);
    EXPECT_NE(strstr(buf, "Type"), (char*)nullptr);
}

TEST(ProcFsTest, CapsNodeShowsCapability) {
    Scheduler::instance().init();

    uint32_t stack[128];
    TaskControlBlock* task = Scheduler::instance().create_task([](){}, stack, sizeof(stack));
    ASSERT_NE(task, nullptr);

    // Insert a capability
    Endpoint ep;
    task->cspace[0].type = auroraos::kernel::CapType::Endpoint;
    task->cspace[0].rights = {true, true, false, 0};
    task->cspace[0].badge = 42;
    task->cspace[0].object = &ep;

    CapsNode node;
    char buf[512] = {0};
    int n = node.read(buf, sizeof(buf), 0, nullptr);

    EXPECT_GT(n, 0);
    EXPECT_NE(strstr(buf, "Endpoint"), (char*)nullptr);
    EXPECT_NE(strstr(buf, "RW-"), (char*)nullptr);
    EXPECT_NE(strstr(buf, "42"), (char*)nullptr);
}

TEST(ProcFsTest, CapsNodeSkipsNullSlots) {
    Scheduler::instance().init();

    uint32_t stack[128];
    TaskControlBlock* task = Scheduler::instance().create_task([](){}, stack, sizeof(stack));
    ASSERT_NE(task, nullptr);

    // All slots should be Null (default init)
    CapsNode node;
    char buf[512] = {0};
    int n = node.read(buf, sizeof(buf), 0, nullptr);

    EXPECT_GT(n, 0);
    // Should only contain header lines, no capability entries
    // Count newlines — header is 2 lines (header + separator)
    int newline_count = 0;
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\n') newline_count++;
    }
    EXPECT_EQ(newline_count, 2); // header + separator only
}

TEST(ProcFsTest, CapsNodeOffsetReturnsZero) {
    CapsNode node;
    char buf[256] = {0};
    int n = node.read(buf, sizeof(buf), 1, nullptr);
    EXPECT_EQ(n, 0);
}
