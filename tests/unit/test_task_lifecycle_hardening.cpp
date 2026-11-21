// =============================================================================
// test_task_lifecycle_hardening.cpp
//
// Regression tests for scheduler / task-lifecycle hardening:
//   - AppControlBlock::transition_to (critical section, unified mutex cleanup,
//     same-state no-op, Unallocated/Terminated TCB rejection)
//   - tick_update stack-canary path routed through terminate_task
//   - schedule() fallback scan bounds-checks corrupted next_ready
//   - remove_ready rebuilds a corrupted ready ring instead of indexing tasks[-N]
//   - set_task_priority / create_task clamp priority to the bucket range
//   - create_task defers reuse of Terminated slots with external references
//   - TaskNotify give/take refuse dead tasks
//   - WaitQueue::remove clears a stale waiting_queue binding
//   - WorkQueue::submit_from_isr rejects null callbacks
// =============================================================================

#include <gtest/gtest.h>

#include "task.hpp"
#include "app_lifecycle.hpp"
#include "task_notify.hpp"
#include "work_queue.hpp"
#include "wait_queue.hpp"
#include "mutex.hpp"

#include <array>
#include <cstdint>

namespace {

constexpr uint32_t kStackWords = 64u;

void dummy_task() {}

class TaskLifecycleHardeningTest : public ::testing::Test {
protected:
    void SetUp() override {
        Scheduler::instance().init();
        g_current_tcb_ptr = nullptr;
        g_next_tcb_ptr = nullptr;
    }

    void TearDown() override {
        g_current_tcb_ptr = nullptr;
        g_next_tcb_ptr = nullptr;
        Scheduler::instance().set_started(false);
    }

    TaskControlBlock* add_task(TaskPriority prio = TaskPriority::Normal) {
        static std::array<uint32_t, kStackWords> stacks[16];
        static int idx = 0;
        idx = (idx + 1) % 16;
        return Scheduler::instance().create_task(dummy_task, stacks[idx].data(), kStackWords * sizeof(uint32_t), prio);
    }
};

// ---------------------------------------------------------------------------
// AppControlBlock::transition_to
// ---------------------------------------------------------------------------
TEST_F(TaskLifecycleHardeningTest, SuspendReleasesAllHeldMutexesThroughKernelPath) {
    TaskControlBlock* const app = add_task();
    ASSERT_NE(app, nullptr);

    Mutex m1;
    Mutex m2;
    g_current_tcb_ptr = app;
    ASSERT_TRUE(m1.lock());
    ASSERT_TRUE(m2.lock());
    ASSERT_NE(app->scheduler.held_mutexes, nullptr);
    g_current_tcb_ptr = nullptr;

    AppControlBlock acb{app->scheduler.id, AppState::BACKGROUND, "app"};
    acb.transition_to(AppState::SUSPENDED);

    EXPECT_EQ(acb.state, AppState::SUSPENDED);
    EXPECT_EQ(app->scheduler.held_mutexes, nullptr);
    EXPECT_EQ(m1.get_next_held(), nullptr);
    EXPECT_EQ(m2.get_next_held(), nullptr);
    EXPECT_EQ(app->scheduler.state, TaskState::Suspended);
}

TEST_F(TaskLifecycleHardeningTest, TransitionRejectedForUnallocatedOrTerminatedTcb) {
    TaskControlBlock* const app = add_task();
    ASSERT_NE(app, nullptr);
    const uint32_t tid = app->scheduler.id;

    Scheduler::instance().free_task(app);
    ASSERT_EQ(app->scheduler.state, TaskState::Unallocated);

    AppControlBlock acb{tid, AppState::NOT_RUNNING, "app"};
    acb.transition_to(AppState::FOREGROUND);
    EXPECT_EQ(acb.state, AppState::NOT_RUNNING) << "ACB must not diverge from an Unallocated TCB";
    EXPECT_EQ(app->scheduler.state, TaskState::Unallocated);

    TaskControlBlock* const app2 = add_task();
    ASSERT_NE(app2, nullptr);
    Scheduler::instance().terminate_task(app2->scheduler.id);
    AppControlBlock acb2{app2->scheduler.id, AppState::BACKGROUND, "app2"};
    acb2.transition_to(AppState::FOREGROUND);
    EXPECT_EQ(acb2.state, AppState::BACKGROUND);
    EXPECT_EQ(app2->scheduler.state, TaskState::Terminated);
}

TEST_F(TaskLifecycleHardeningTest, SameStateTransitionIsNoOp) {
    TaskControlBlock* const app = add_task(TaskPriority::Normal);
    ASSERT_NE(app, nullptr);

    // The ACB claims BACKGROUND while the TCB still runs at Normal: a
    // same-state transition must not touch the TCB at all.
    AppControlBlock acb{app->scheduler.id, AppState::BACKGROUND, "app"};
    acb.transition_to(AppState::BACKGROUND);
    EXPECT_EQ(app->scheduler.current_priority, TaskPriority::Normal);

    acb.transition_to(AppState::FOREGROUND);
    EXPECT_EQ(acb.state, AppState::FOREGROUND);
    EXPECT_EQ(app->scheduler.current_priority, TaskPriority::Realtime);
}

// ---------------------------------------------------------------------------
// tick_update stack canary → terminate_task
// ---------------------------------------------------------------------------
TEST_F(TaskLifecycleHardeningTest, CanaryViolationReleasesResourcesViaTerminateTask) {
    TaskControlBlock* const victim = add_task();
    ASSERT_NE(victim, nullptr);

    Mutex m;
    g_current_tcb_ptr = victim;
    ASSERT_TRUE(m.lock());
    g_current_tcb_ptr = nullptr;
    ASSERT_NE(victim->scheduler.held_mutexes, nullptr);

    static uint32_t smashed = 0u;
    victim->stack_canary_ptr = &smashed;

    Scheduler::instance().tick_update();

    EXPECT_EQ(victim->scheduler.state, TaskState::Terminated);
    EXPECT_EQ(victim->scheduler.held_mutexes, nullptr) << "canary kill must release held mutexes";
}

// ---------------------------------------------------------------------------
// schedule() fallback scan with corrupted next_ready
// ---------------------------------------------------------------------------
TEST_F(TaskLifecycleHardeningTest, ScheduleFallbackStopsOnOutOfRangeNextReady) {
    TaskControlBlock* const a = add_task(TaskPriority::Normal);
    TaskControlBlock* const b = add_task(TaskPriority::Normal);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    // a is the Normal-bucket head. Corrupt it: non-Ready while still linked and
    // with next_ready = -2 (≠ -1 after the uint32_t cast).
    a->scheduler.state = TaskState::Sleeping;
    a->scheduler.next_ready = -2;

    g_current_tcb_ptr = a;
    Scheduler::instance().set_started(true);
    Scheduler::instance().schedule(); // must not index tasks[0xFFFFFFFE]

    EXPECT_EQ(g_next_tcb_ptr, nullptr) << "no valid Ready task reachable → no switch";
}

// ---------------------------------------------------------------------------
// remove_ready with corrupted prev_ready
// ---------------------------------------------------------------------------
TEST_F(TaskLifecycleHardeningTest, RemoveReadyRebuildsRingOnCorruptPrevLink) {
    TaskControlBlock* const a = add_task(TaskPriority::Normal);
    TaskControlBlock* const b = add_task(TaskPriority::Normal);
    TaskControlBlock* const c = add_task(TaskPriority::Normal);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    b->scheduler.prev_ready = -5;
    Scheduler::instance().set_task_state(b->scheduler.id, TaskState::Suspended);

    EXPECT_EQ(b->scheduler.next_ready, -1);
    EXPECT_EQ(b->scheduler.prev_ready, -1);
    // Ring rebuilt as a <-> c.
    EXPECT_EQ(a->scheduler.next_ready, static_cast<int32_t>(c->scheduler.id));
    EXPECT_EQ(c->scheduler.next_ready, static_cast<int32_t>(a->scheduler.id));
    EXPECT_EQ(Scheduler::instance().get_highest_ready_priority(), static_cast<int32_t>(TaskPriority::Normal));

    Scheduler::instance().set_task_state(a->scheduler.id, TaskState::Suspended);
    Scheduler::instance().set_task_state(c->scheduler.id, TaskState::Suspended);
    EXPECT_EQ(Scheduler::instance().get_highest_ready_priority(), -1);
}

// ---------------------------------------------------------------------------
// Priority clamping
// ---------------------------------------------------------------------------
TEST_F(TaskLifecycleHardeningTest, OutOfRangePriorityIsClampedAndStaysRemovable) {
    TaskControlBlock* const t = add_task(static_cast<TaskPriority>(200));
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->scheduler.current_priority, TaskPriority::Realtime);

    Scheduler::instance().set_task_priority(t->scheduler.id, TaskPriority::Normal);
    Scheduler::instance().set_task_priority(t->scheduler.id, static_cast<TaskPriority>(77));
    EXPECT_EQ(t->scheduler.current_priority, TaskPriority::Realtime);
    EXPECT_EQ(Scheduler::instance().get_highest_ready_priority(), static_cast<int32_t>(TaskPriority::Realtime));

    Scheduler::instance().set_task_state(t->scheduler.id, TaskState::Suspended);
    EXPECT_EQ(Scheduler::instance().get_highest_ready_priority(), -1)
        << "task must not linger in a bucket that differs from current_priority";
}

// ---------------------------------------------------------------------------
// Deferred reuse of referenced Terminated slots
// ---------------------------------------------------------------------------
TEST_F(TaskLifecycleHardeningTest, TerminatedSlotWithExternalRefIsNotReused) {
    TaskControlBlock* const a = add_task();
    ASSERT_NE(a, nullptr);
    Scheduler::instance().terminate_task(a->scheduler.id);
    a->retain(); // e.g. another task's Thread capability still points here

    TaskControlBlock* const b = add_task();
    ASSERT_NE(b, nullptr);
    EXPECT_NE(b, a);
    EXPECT_EQ(a->scheduler.state, TaskState::Terminated);
    EXPECT_EQ(a->get_ref_count(), 2u);

    a->release();
    TaskControlBlock* const c = add_task();
    EXPECT_EQ(c, a) << "slot becomes reusable once the external reference is dropped";
}

// ---------------------------------------------------------------------------
// TaskNotify
// ---------------------------------------------------------------------------
TEST_F(TaskLifecycleHardeningTest, NotifyGiveToTerminatedTaskFails) {
    TaskControlBlock* const t = add_task();
    ASSERT_NE(t, nullptr);
    Scheduler::instance().terminate_task(t->scheduler.id);

    EXPECT_FALSE(TaskNotify::give(t->scheduler.id, 0x5Au, false));
    EXPECT_FALSE(t->ipc.notify_pending);
}

TEST_F(TaskLifecycleHardeningTest, NotifyTakeOnTerminatedCurrentReturnsWithoutBlocking) {
    TaskControlBlock* const t = add_task();
    ASSERT_NE(t, nullptr);
    Scheduler::instance().terminate_task(t->scheduler.id);
    g_current_tcb_ptr = t;

    EXPECT_EQ(TaskNotify::take(), 0u);
    EXPECT_EQ(t->scheduler.state, TaskState::Terminated);
}

// ---------------------------------------------------------------------------
// WaitQueue stale binding
// ---------------------------------------------------------------------------
TEST_F(TaskLifecycleHardeningTest, WaitQueueRemoveClearsStaleBinding) {
    TaskControlBlock* const t = add_task();
    TaskControlBlock* const u = add_task();
    ASSERT_NE(t, nullptr);
    ASSERT_NE(u, nullptr);

    auroraos::kernel::WaitQueue q;
    t->scheduler.waiting_queue = &q;
    EXPECT_FALSE(q.remove(t));
    EXPECT_EQ(t->scheduler.waiting_queue, nullptr);

    q.enqueue(u);
    t->scheduler.waiting_queue = &q;
    EXPECT_FALSE(q.remove(t));
    EXPECT_EQ(t->scheduler.waiting_queue, nullptr);
    EXPECT_TRUE(q.contains(u));

    q.enqueue(t);
    EXPECT_TRUE(q.contains(t)) << "task must be re-enqueueable after the stale binding is cleared";
    q.clear();
}

// ---------------------------------------------------------------------------
// WorkQueue
// ---------------------------------------------------------------------------
TEST_F(TaskLifecycleHardeningTest, WorkQueueRejectsNullCallback) {
    EXPECT_FALSE(WorkQueue::instance().submit_from_isr(nullptr));
}

} // namespace
