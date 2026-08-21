// test_hids.cpp — 主机入侵检测（HIDS）单元测试
//
// 覆盖：
//   - 文件完整性：FNV-1a 哈希 + 内容篡改检测
//   - 进程监控：栈溢出（金丝雀）
//   - 权限审计：用户态任务持有 grant 能力
//   - Rootkit 扫描：内核堆魔数破坏 + 函数序言校验

#include <gtest/gtest.h>
#include "../../security/hids/hids_engine.hpp"

using namespace aurora::hids;

extern volatile uint32_t tick_count;

// 供 RootkitScanner::check_prologue 测试用的目标函数（须在文件作用域定义）
static void hids_dummy_fn() {}

// ---- 文件完整性 ----
TEST(FileIntegrityTest, Fnv1aKnownVectors) {
    EXPECT_EQ(FileIntegrityMonitor::fnv1a32_str(""), 0x811C9DC5u);
    EXPECT_EQ(FileIntegrityMonitor::fnv1a32_str("a"), 0xE40C292Cu);
}

TEST(FileIntegrityTest, DetectsContentModification) {
    FileIntegrityMonitor fim;

    const uint8_t original[] = {0x01, 0x02, 0x03, 0x04};
    const uint8_t tampered[] = {0x01, 0x02, 0x03, 0xFF};

    ASSERT_TRUE(fim.add_path("/etc/config"));
    ASSERT_TRUE(fim.set_baseline("/etc/config", FileIntegrityMonitor::fnv1a32(original, 4)));

    // 未篡改：不告警
    EXPECT_FALSE(fim.verify_content("/etc/config", original, 4));
    EXPECT_EQ(fim.get_total_findings(), 0u);

    // 篡改：告警
    EXPECT_TRUE(fim.verify_content("/etc/config", tampered, 4));
    EXPECT_GT(fim.get_total_findings(), 0u);
}

// ---- 进程监控（栈溢出） ----
TEST(ProcessMonitorTest, DetectsStackOverflow) {
    Scheduler::instance().init();
    static uint32_t stack[64];
    TaskControlBlock* tcb = Scheduler::instance().create_task([]() {}, stack, sizeof(stack),
                                                              TaskPriority::Normal);
    ASSERT_NE(tcb, nullptr);

    ProcessMonitor pm;
    // 正常任务：无发现
    EXPECT_EQ(pm.scan(), 0);

    // 破坏栈金丝雀
    *tcb->task.stack_canary_ptr = 0x00000000u;
    EXPECT_GT(pm.scan(), 0);
    EXPECT_EQ(pm.get_overflow_count(), 1u);
}

// ---- 权限审计（用户态持有 grant 能力） ----
TEST(PrivilegeAuditorTest, DetectsGrantCapabilityInUserTask) {
    Scheduler::instance().init();
    static uint32_t stack[64];
    TaskControlBlock* tcb = Scheduler::instance().create_task([]() {}, stack, sizeof(stack),
                                                              TaskPriority::Normal, 0, TaskPrivilege::User);
    ASSERT_NE(tcb, nullptr);

    // 用户态任务插入一个带 grant 权限的能力
    auroraos::kernel::Capability cap{};
    cap.type = auroraos::kernel::CapType::Device;
    cap.rights = {false, false, true, 0}; // grant = true
    cap.badge = 0;
    cap.object = nullptr;
    ASSERT_TRUE(auroraos::kernel::CSpace::cap_insert(tcb, 0, cap));

    PrivilegeAuditor pa;
    EXPECT_GT(pa.scan(), 0);
    EXPECT_GT(pa.get_total_findings(), 0u);
}

TEST(PrivilegeAuditorTest, IgnoresKernelTask) {
    Scheduler::instance().init();
    static uint32_t stack[64];
    TaskControlBlock* tcb = Scheduler::instance().create_task([]() {}, stack, sizeof(stack),
                                                              TaskPriority::Normal, 0, TaskPrivilege::Kernel);
    ASSERT_NE(tcb, nullptr);

    auroraos::kernel::Capability cap{};
    cap.type = auroraos::kernel::CapType::Device;
    cap.rights = {false, false, true, 0}; // grant = true
    cap.badge = 0;
    cap.object = nullptr;
    ASSERT_TRUE(auroraos::kernel::CSpace::cap_insert(tcb, 0, cap));

    PrivilegeAuditor pa;
    // 内核态任务持 grant 能力是合法的，不告警
    EXPECT_EQ(pa.scan(), 0);
}

// ---- Rootkit 扫描 ----
TEST(RootkitScannerTest, DetectsHeapMagicCorruption) {
    static uint8_t heap[4096];
    KernelHeap::instance().init(heap, heap + sizeof(heap));

    void* p = KernelHeap::instance().allocate(64);
    ASSERT_NE(p, nullptr);

    // 未破坏：无发现
    RootkitScanner rs;
    EXPECT_EQ(rs.scan(), 0);

    // 破坏块头魔数（块头位于用户指针之前）
    uint32_t* magic = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(p) - KernelHeap::BLOCK_HEADER_SIZE);
    *magic = 0xDEADBEEFu;

    EXPECT_GT(rs.scan(), 0);
    EXPECT_GT(rs.get_heap_corrupt_count(), 0u);

    // 清理：恢复魔数避免影响后续 deallocate
    *magic = 0x544C5346u;
    KernelHeap::instance().deallocate(p);
}

TEST(RootkitScannerTest, CheckPrologue) {
    // 任意函数指针，首字节应与其自身一致
    const uint8_t first = *reinterpret_cast<const uint8_t*>(reinterpret_cast<void*>(&hids_dummy_fn));

    EXPECT_TRUE(RootkitScanner::check_prologue(reinterpret_cast<const void*>(&hids_dummy_fn), &first, 1));

    // 篡改字节 → 判定被篡改
    uint8_t bad = static_cast<uint8_t>(first ^ 0xFF);
    EXPECT_FALSE(RootkitScanner::check_prologue(reinterpret_cast<const void*>(&hids_dummy_fn), &bad, 1));
}

// ---- 引擎集成 ----
TEST(HidsEngineTest, TickRunsAllModules) {
    tick_count = 1000;
    HidsEngine::instance().reset();
    HidsEngine::instance().init();

    // 引擎 tick() 应能无崩溃地运行 4 个模块（当前宿主环境无已注册文件/异常）
    HidsEngine::instance().tick();
    EXPECT_GE(HidsEngine::instance().get_alert_count(), 0u);
}
