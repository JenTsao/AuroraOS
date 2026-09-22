#include <gtest/gtest.h>
#include <cstring>

#define private public
#include "task/task.hpp"
#include "core/process_timer.hpp"
#include "interrupt/timer.hpp"
#include "core/cspace.hpp"
#include "mm/memory_protection.hpp"
#include "../../vfs/procfs.hpp"
#undef private

using namespace auroraos::kernel;

class JulyKernelCoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        Scheduler::instance().init();
        ProcessTimerManager::instance().init();
        // Create idle task (TID 0)
        Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Idle);
        Scheduler::instance().set_started(true);
    }
};

// =============================================================================
// Pillar 1: KernelObject & Capability 2.0 (ProcessTimer Cap integration)
// =============================================================================

TEST_F(JulyKernelCoreTest, TimerCapabilityCreationAndLookup) {
    uint32_t stack[128];
    TaskControlBlock* task = Scheduler::instance().create_task([]() {}, stack, sizeof(stack), TaskPriority::Normal);
    ASSERT_NE(task, nullptr);

    ProcessTimerDesc desc{};
    desc.flags = TimerFlags::OneShot | TimerFlags::Relative | TimerFlags::NotifySignal;
    desc.initial_delay_ms = 50;
    desc.interval_ms = 0;
    desc.notify_param = 14;

    int slot = ProcessTimerManager::instance().create_timer_cap(task, &desc);
    EXPECT_GE(slot, 0);

    Capability* cap = CSpace::cap_lookup(task, static_cast<uint32_t>(slot));
    ASSERT_NE(cap, nullptr);
    EXPECT_EQ(cap->type, CapType::Timer);
    EXPECT_TRUE(cap->rights.read);
    EXPECT_TRUE(cap->rights.write);
    EXPECT_TRUE(cap->rights.grant);
    ASSERT_NE(cap->object, nullptr);
    EXPECT_EQ(cap->object->get_type(), ObjectType::Timer);

    // Retrieve timer via capability
    ProcessTimer* timer = ProcessTimerManager::instance().get_timer_by_cap(task, static_cast<uint32_t>(slot), CAP_RIGHT_READ | CAP_RIGHT_WRITE);
    ASSERT_NE(timer, nullptr);
    EXPECT_TRUE(timer->allocated);
    EXPECT_EQ(timer->owner_task_id, task->scheduler.id);
    EXPECT_EQ(timer->notify_param, 14u);
}

TEST_F(JulyKernelCoreTest, TimerCapabilityRightsAttenuation) {
    uint32_t stack[128];
    TaskControlBlock* task = Scheduler::instance().create_task([]() {}, stack, sizeof(stack), TaskPriority::Normal);
    ASSERT_NE(task, nullptr);

    ProcessTimerDesc desc{};
    desc.flags = TimerFlags::Periodic | TimerFlags::Relative | TimerFlags::NotifySignal;
    desc.initial_delay_ms = 10;
    desc.interval_ms = 10;
    desc.notify_param = 14;

    int src_slot = ProcessTimerManager::instance().create_timer_cap(task, &desc);
    ASSERT_GE(src_slot, 0);

    int dst_slot = CSpace::cap_alloc_slot(task);
    ASSERT_GE(dst_slot, 0);

    // Derive read-only timer capability
    bool derived = CSpace::cap_derive(task, static_cast<uint32_t>(src_slot), static_cast<uint32_t>(dst_slot), CAP_RIGHT_READ);
    EXPECT_TRUE(derived);

    // Read access should succeed
    ProcessTimer* ro_timer = ProcessTimerManager::instance().get_timer_by_cap(task, static_cast<uint32_t>(dst_slot), CAP_RIGHT_READ);
    EXPECT_NE(ro_timer, nullptr);

    // Write access should fail on read-only capability
    ProcessTimer* rw_timer = ProcessTimerManager::instance().get_timer_by_cap(task, static_cast<uint32_t>(dst_slot), CAP_RIGHT_WRITE);
    EXPECT_EQ(rw_timer, nullptr);
}

TEST_F(JulyKernelCoreTest, TimerCapabilityCleanupOnTaskFree) {
    uint32_t stack[128];
    TaskControlBlock* task = Scheduler::instance().create_task([]() {}, stack, sizeof(stack), TaskPriority::Normal);
    ASSERT_NE(task, nullptr);

    ProcessTimerDesc desc{};
    desc.flags = TimerFlags::Periodic | TimerFlags::Relative | TimerFlags::NotifySignal;
    desc.initial_delay_ms = 10;
    desc.interval_ms = 10;
    desc.notify_param = 14;

    int slot = ProcessTimerManager::instance().create_timer_cap(task, &desc);
    ASSERT_GE(slot, 0);

    ProcessTimer* timer = ProcessTimerManager::instance().get_timer_by_cap(task, static_cast<uint32_t>(slot));
    ASSERT_NE(timer, nullptr);
    EXPECT_TRUE(timer->allocated);

    // Free the task
    Scheduler::instance().free_task(task);

    // Timer slot should be unallocated and CSpace slots should be cleared
    EXPECT_FALSE(timer->allocated);
    EXPECT_EQ(task->security.occupied_mask, 0);
}

// =============================================================================
// Pillar 2: Scheduler CPU Accounting & Metrics
// =============================================================================

TEST_F(JulyKernelCoreTest, SchedulerCpuMetricsAccounting) {
    uint32_t stack1[128];
    uint32_t stack2[128];

    TaskControlBlock* task1 = Scheduler::instance().create_task([]() {}, stack1, sizeof(stack1), TaskPriority::High);
    TaskControlBlock* task2 = Scheduler::instance().create_task([]() {}, stack2, sizeof(stack2), TaskPriority::Normal);
    ASSERT_NE(task1, nullptr);
    ASSERT_NE(task2, nullptr);

    // Initial metrics should be 0
    Scheduler::TaskMetrics metrics{};
    EXPECT_TRUE(Scheduler::instance().get_task_metrics(task1->scheduler.id, metrics));
    EXPECT_EQ(metrics.switch_count, 0u);
    EXPECT_EQ(metrics.runtime_ticks, 0u);

    // Trigger schedule
    Scheduler::instance().schedule();

    // The chosen task should have switch_count >= 1
    EXPECT_TRUE(Scheduler::instance().get_task_metrics(task1->scheduler.id, metrics));
    EXPECT_GE(metrics.switch_count, 1u);
    EXPECT_GE(Scheduler::instance().get_total_switches(), 1u);

    // Simulate clock ticks
    for (int i = 0; i < 10; i++) {
        Scheduler::instance().tick_update();
    }

    EXPECT_GT(Scheduler::instance().get_active_ticks() + Scheduler::instance().get_idle_ticks(), 0u);
    uint32_t load = Scheduler::instance().get_cpu_load();
    EXPECT_LE(load, 100u);
}

// =============================================================================
// Pillar 3: Unified Memory Protection Interface (IMemoryProtection)
// =============================================================================

TEST_F(JulyKernelCoreTest, UnifiedMemoryProtectionManager) {
    MemoryProtectionManager& mem_mgr = MemoryProtectionManager::instance();
    EXPECT_TRUE(mem_mgr.is_active());

    ProtectionModel model = mem_mgr.get_model();
    EXPECT_TRUE(model == ProtectionModel::Mpu || model == ProtectionModel::Mmu);

    uint32_t stack[256];
    TaskControlBlock* task = Scheduler::instance().create_task([]() {}, stack, sizeof(stack), TaskPriority::Normal, 10);
    ASSERT_NE(task, nullptr);

    // Stack protection configuration
    bool prot_ok = mem_mgr.protect_task_stack(task);
    EXPECT_TRUE(prot_ok);
    EXPECT_TRUE(task->memory.mpu_sandbox.is_valid());

    // Sandbox configuration
    bool sb_ok = mem_mgr.configure_sandbox(task, reinterpret_cast<uintptr_t>(stack), sizeof(stack), MemoryPermission::ReadWrite);
    EXPECT_TRUE(sb_ok);
    EXPECT_TRUE(task->memory.mpu_sandbox.is_valid());

    // Validate user buffer on task's stack
    bool valid_buf = mem_mgr.validate_user_buffer(task, stack, 16, true);
    EXPECT_TRUE(valid_buf);

    // Null pointer should be rejected
    bool null_buf = mem_mgr.validate_user_buffer(task, nullptr, 16, false);
    EXPECT_FALSE(null_buf);

    // Size 0 should be rejected
    bool zero_buf = mem_mgr.validate_user_buffer(task, stack, 0, false);
    EXPECT_FALSE(zero_buf);
}

// =============================================================================
// Pillar 4: ProcFS CPU & Caps Integration
// =============================================================================

TEST_F(JulyKernelCoreTest, CpuInfoNodeShowsMetrics) {
    CpuInfoNode node;
    char buf[256] = {0};
    int n = node.read(buf, sizeof(buf), 0, nullptr);
    EXPECT_GT(n, 0);
    EXPECT_NE(strstr(buf, "auroraOS CPU & Scheduler Info:"), nullptr);
    EXPECT_NE(strstr(buf, "CPULoad:"), nullptr);
    EXPECT_NE(strstr(buf, "TotalSwitches:"), nullptr);
}

TEST_F(JulyKernelCoreTest, CapsNodeDisplaysTimerCapability) {
    uint32_t stack[128];
    TaskControlBlock* task = Scheduler::instance().create_task([]() {}, stack, sizeof(stack), TaskPriority::Normal);
    ASSERT_NE(task, nullptr);

    ProcessTimerDesc desc{};
    desc.flags = TimerFlags::OneShot | TimerFlags::Relative | TimerFlags::NotifySignal;
    desc.initial_delay_ms = 100;
    desc.interval_ms = 0;
    desc.notify_param = 14;

    int slot = ProcessTimerManager::instance().create_timer_cap(task, &desc);
    ASSERT_GE(slot, 0);

    CapsNode node;
    char buf[512] = {0};
    int n = node.read(buf, sizeof(buf), 0, nullptr);
    EXPECT_GT(n, 0);
    EXPECT_NE(strstr(buf, "Timer"), nullptr);
}
