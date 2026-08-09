#include <gtest/gtest.h>
#include "mutex.hpp"
#include "mutex.hpp"
#include "frame_scheduler_v2.hpp"
#include "timer.hpp"

void arch_test_interrupt_hook_impl() {
    // Advance time by 1 tick so that timeouts can occur when tasks are blocked
    TimerManager::instance().fast_forward_ticks(1);
    Scheduler::instance().tick_update();
    if (TimerManager::instance().get_current_tick() % 100000 == 0) {
        printf("Tick: %u\n", TimerManager::instance().get_current_tick());
    }
}

class MutexTest : public ::testing::Test {
protected:
    void SetUp() override {
        Arch::g_arch_test_interrupt_hook = arch_test_interrupt_hook_impl;
        Scheduler::instance().init();
        // Create an idle task so the scheduler has something to fall back to
        Scheduler::instance().create_task([](){}, nullptr, 0, TaskPriority::Idle);
        
        // Disable UI render window so normal tasks can be scheduled
        FrameSchedulerV2::instance().notify_render_complete();
        
        // Start scheduler so schedule() actually works
        Scheduler::instance().set_started(true);
    }
    
    void TearDown() override {
    }
};

// Test recursive lock
TEST_F(MutexTest, RecursiveLock) {
    Mutex m;
    
    // Simulate being in a task context
    (void)Scheduler::instance().create_task([](){}, nullptr, 0, TaskPriority::Normal);
    Scheduler::instance().schedule(); // Switch to the new task
    
    // First lock
    EXPECT_TRUE(m.lock());
    
    // Recursive lock
    EXPECT_TRUE(m.lock());
    
    // Recursive lock
    EXPECT_TRUE(m.lock());
    
    // Unlock once
    m.unlock();
    
    // Should still be locked (recursion depth 2)
    
    // Unlock twice
    m.unlock();
    
    // Unlock fully
    m.unlock();
}

// Test timeout logic
TEST_F(MutexTest, LockTimeout) {
    Mutex m;
    
    // task1 takes the lock
    TaskControlBlock* task1 = Scheduler::instance().create_task([](){}, nullptr, 0, TaskPriority::Normal);
    Scheduler::instance().schedule(); // Switch to task1
    EXPECT_TRUE(m.lock());
    
    // Suspend task1 so task2 becomes the active task
    Scheduler::instance().set_task_state(task1->id, TaskState::Suspended);

    // task2 tries to take the lock with a timeout
    TaskControlBlock* task2 = Scheduler::instance().create_task([](){}, nullptr, 0, TaskPriority::Normal);
    Scheduler::instance().schedule(); // Switch to task2
    
    bool acquired = m.lock(5); 
    EXPECT_FALSE(acquired); // task1 owns it, so task2 should timeout
    
    // Suspend task2, resume task1
    Scheduler::instance().set_task_state(task2->id, TaskState::Suspended);
    Scheduler::instance().set_task_state(task1->id, TaskState::Ready);
    Scheduler::instance().schedule(); // Switch to task1
    m.unlock();
    
    // Resume task2 to let it acquire
    Scheduler::instance().set_task_state(task1->id, TaskState::Suspended);
    Scheduler::instance().set_task_state(task2->id, TaskState::Ready);
    Scheduler::instance().schedule(); // Switch to task2
    EXPECT_TRUE(m.lock(5));
}

// Test UniqueLock RAII
TEST_F(MutexTest, UniqueLockRAII) {
    Mutex m;
    
    TaskControlBlock* task1 = Scheduler::instance().create_task([](){}, nullptr, 0, TaskPriority::Normal);
    Scheduler::instance().schedule(); // Switch to task1

    {
        UniqueLock lock(m, 10);
        EXPECT_TRUE(lock.owns_lock());
        
        // Recursive lock using UniqueLock
        UniqueLock recursive_lock(m, 10);
        EXPECT_TRUE(recursive_lock.owns_lock());
    } // Both destructors run, lock is fully released
    
    // Suspend task1 so task2 is active
    Scheduler::instance().set_task_state(task1->id, TaskState::Suspended);

    // Task 2 can now acquire
    (void)Scheduler::instance().create_task([](){}, nullptr, 0, TaskPriority::Normal);
    Scheduler::instance().schedule(); // Switch to task2
    UniqueLock lock2(m, 5);
    EXPECT_TRUE(lock2.owns_lock());
}

// Test priority inheritance timeout revert
TEST_F(MutexTest, PITimeoutRevert) {
    Mutex m;
    
    TaskControlBlock* task1 = Scheduler::instance().create_task([](){}, nullptr, 0, TaskPriority::Low);
    Scheduler::instance().schedule();
    m.lock();
    EXPECT_EQ(task1->current_priority, TaskPriority::Low);
    
    Scheduler::instance().set_task_state(task1->id, TaskState::Suspended);
    
    (void)Scheduler::instance().create_task([](){}, nullptr, 0, TaskPriority::High);
    Scheduler::instance().schedule();
    
    // task2 waits, elevates task1 (High), then times out
    EXPECT_FALSE(m.lock(2));
    
    // After timeout, task1 should revert back to Low!
    EXPECT_EQ(task1->current_priority, TaskPriority::Low);
}
