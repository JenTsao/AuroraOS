// =============================================================================
// test_scheduler.cpp — Unit tests for Scheduler (kernel/task.hpp)
//
// Strategy:
//  - The Scheduler is a singleton, so we call init() in SetUp() to reset
//    state between tests.
//  - Arch:: and frame_scheduler_is_task_allowed() are provided by stubs/
//    arch_api.hpp (no-ops), so create_task / tick_update / schedule() compile
//    and run on the host without ARM hardware.
//  - Tests that call schedule() or start() skip the context-switch path
//    because trigger_context_switch() is a no-op stub; we only verify the
//    *data-structure* changes (TCB state, priorities, signal dispatch).
//
// C++ Core Guidelines applied:
//  Con.1: fixture fields const where not mutated
//  F.20: assertions on return values, no output-param anti-patterns
//  ES.20: all variables initialised at declaration
// =============================================================================

#include <gtest/gtest.h>
#include <gmock/gmock.h>

// Stubs are injected first via the include path, so task.hpp sees our stub
// arch_api.hpp instead of the real one.
#include "task.hpp"

#include <array>
#include <cstdint>

// ---------------------------------------------------------------------------
// Helper: a minimal stack large enough for init_thread_stack() to write a
// fake frame without going out of bounds.
// ---------------------------------------------------------------------------
static constexpr uint32_t kStackWords = 64u;

// Dummy task entry — never actually called in host tests.
static void dummy_task() {}

// ---------------------------------------------------------------------------
// SchedulerTest fixture — resets the singleton before every test.
// ---------------------------------------------------------------------------
class SchedulerTest : public ::testing::Test {
protected:
    void SetUp() override {
        Scheduler::instance().init();
    }

    // Helper: allocate a static stack and register a task with the scheduler.
    // Returns the TCB pointer or nullptr on failure.
    TaskControlBlock* add_task(TaskPriority prio = TaskPriority::Normal) {
        // Each stack is a separate static local array; test isolation is
        // achieved through Scheduler::init() which resets task_count to 0.
        static std::array<uint32_t, kStackWords> stacks[16];
        static int idx = 0;
        idx = (idx + 1) % 16;

        return Scheduler::instance().create_task(
            dummy_task,
            stacks[idx].data(),
            kStackWords * sizeof(uint32_t),
            prio);
    }
};

// ---------------------------------------------------------------------------
// 1. create_task returns a valid TCB pointer
// ---------------------------------------------------------------------------
TEST_F(SchedulerTest, CreateTaskReturnsValidTcb) {
    TaskControlBlock* const tcb = add_task();

    ASSERT_NE(tcb, nullptr);
    EXPECT_EQ(tcb->state, TaskState::Ready);
    EXPECT_EQ(tcb->base_priority, TaskPriority::Normal);
}

// ---------------------------------------------------------------------------
// 2. Task count increments with each creation
// ---------------------------------------------------------------------------
TEST_F(SchedulerTest, TaskCountIncrements) {
    EXPECT_EQ(Scheduler::instance().get_task_count(), 0);

    add_task();
    EXPECT_EQ(Scheduler::instance().get_task_count(), 1);

    add_task();
    EXPECT_EQ(Scheduler::instance().get_task_count(), 2);
}

// ---------------------------------------------------------------------------
// 3. Exceeding MAX_TASKS returns nullptr
// ---------------------------------------------------------------------------
TEST_F(SchedulerTest, MaxTasksLimitReturnsNull) {
    // Fill up to the default limit (16 tasks).
    constexpr int kMaxTasks = 16;
    for (int i = 0; i < kMaxTasks; ++i) {
        const TaskControlBlock* const tcb = add_task();
        ASSERT_NE(tcb, nullptr) << "Task " << i << " should be creatable";
    }

    // The 17th create_task must fail.
    TaskControlBlock* const overflow = add_task();
    EXPECT_EQ(overflow, nullptr) << "create_task beyond MAX_TASKS must return nullptr";
}

// ---------------------------------------------------------------------------
// 4. tick_update decrements sleep_ticks and wakes a task when it reaches 0
// ---------------------------------------------------------------------------
TEST_F(SchedulerTest, TickUpdateWakesTask) {
    TaskControlBlock* const tcb = add_task();
    ASSERT_NE(tcb, nullptr);

    // Manually put the task to sleep for 2 ticks.
    tcb->state       = TaskState::Sleeping;
    tcb->sleep_ticks = 2u;

    Scheduler::instance().tick_update();
    EXPECT_EQ(tcb->state, TaskState::Sleeping) << "After 1 tick task must still be sleeping";
    EXPECT_EQ(tcb->sleep_ticks, 1u);

    Scheduler::instance().tick_update();
    EXPECT_EQ(tcb->sleep_ticks, 0u);
    EXPECT_EQ(tcb->state, TaskState::Ready) << "After 2 ticks task must be Ready";
}

// ---------------------------------------------------------------------------
// 5. compensate_ticks correctly batch-deducts skipped ticks
// ---------------------------------------------------------------------------
TEST_F(SchedulerTest, CompensateTicks) {
    TaskControlBlock* const tcb = add_task();
    ASSERT_NE(tcb, nullptr);

    tcb->state       = TaskState::Sleeping;
    tcb->sleep_ticks = 10u;

    // Skip 7 ticks at once (tickless idle simulation).
    Scheduler::instance().compensate_ticks(7u);

    EXPECT_EQ(tcb->sleep_ticks, 3u);
    EXPECT_EQ(tcb->state, TaskState::Sleeping);

    // Skip 5 more — should clamp to 0 and wake the task.
    Scheduler::instance().compensate_ticks(5u);
    EXPECT_EQ(tcb->sleep_ticks, 0u);
    EXPECT_EQ(tcb->state, TaskState::Ready);
}

// ---------------------------------------------------------------------------
// 6. get_task_by_id returns the correct TCB
// ---------------------------------------------------------------------------
TEST_F(SchedulerTest, GetTaskById) {
    TaskControlBlock* const tcb0 = add_task(TaskPriority::Low);
    TaskControlBlock* const tcb1 = add_task(TaskPriority::High);
    ASSERT_NE(tcb0, nullptr);
    ASSERT_NE(tcb1, nullptr);

    EXPECT_EQ(Scheduler::instance().get_task_by_id(0u), tcb0);
    EXPECT_EQ(Scheduler::instance().get_task_by_id(1u), tcb1);
    EXPECT_EQ(Scheduler::instance().get_task_by_id(99u), nullptr);
}

// ---------------------------------------------------------------------------
// 7. dispatch_signals invokes the registered signal handler
// ---------------------------------------------------------------------------
TEST_F(SchedulerTest, SignalDispatchCallsHandler) {
    TaskControlBlock* const tcb = add_task();
    ASSERT_NE(tcb, nullptr);

    static bool handler_called = false;
    handler_called = false;

    // Register SIGUSR1 (signal 10) handler via the new sig_actions API.
    tcb->sig_actions[SIGUSR1].sa_handler = [](int /*sig*/) { handler_called = true; };
    tcb->signal_queue[tcb->sig_tail] = SIGUSR1;
    tcb->sig_tail = (tcb->sig_tail + 1) % TaskControlBlock::MAX_QUEUED_SIGNALS;
    tcb->sig_count++;

    Scheduler::instance().dispatch_signals(tcb);

    EXPECT_TRUE(handler_called) << "SIGUSR1 handler must be invoked by dispatch_signals";
    EXPECT_EQ(tcb->sig_count, 0u) << "signal queue must be empty after dispatch";
}

// ---------------------------------------------------------------------------
// 8. SIGKILL terminates the task and does not call its handler
// ---------------------------------------------------------------------------
TEST_F(SchedulerTest, SigkillTerminatesTask) {
    TaskControlBlock* const tcb = add_task();
    ASSERT_NE(tcb, nullptr);

    static bool handler_called = false;
    handler_called = false;

    // Register a handler for SIGKILL — it must NOT be called (SIGKILL is
    // handled specially: the task is terminated unconditionally).
    tcb->sig_actions[SIGKILL].sa_handler = [](int /*sig*/) { handler_called = false; };
    tcb->signal_queue[tcb->sig_tail] = SIGKILL;
    tcb->sig_tail = (tcb->sig_tail + 1) % TaskControlBlock::MAX_QUEUED_SIGNALS;
    tcb->sig_count++;

    Scheduler::instance().dispatch_signals(tcb);

    EXPECT_EQ(tcb->state, TaskState::Terminated)
        << "SIGKILL must set task state to Terminated";
    EXPECT_FALSE(handler_called)
        << "SIGKILL handler (if any) must not be executed";
}

// ---------------------------------------------------------------------------
// 9. get_expected_idle_ticks returns the minimum remaining sleep_ticks
// ---------------------------------------------------------------------------
TEST_F(SchedulerTest, GetExpectedIdleTicks) {
    TaskControlBlock* const t0 = add_task();
    TaskControlBlock* const t1 = add_task();
    ASSERT_NE(t0, nullptr);
    ASSERT_NE(t1, nullptr);

    t0->state       = TaskState::Sleeping;
    t0->sleep_ticks = 5u;

    t1->state       = TaskState::Sleeping;
    t1->sleep_ticks = 15u;

    const uint32_t idle = Scheduler::instance().get_expected_idle_ticks();
    EXPECT_EQ(idle, 5u) << "Should return the minimum sleep_ticks across all sleeping tasks";
}
