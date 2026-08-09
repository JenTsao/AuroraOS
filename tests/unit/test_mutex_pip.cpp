#include <gtest/gtest.h>

#define private public
#include "mutex.hpp"
#undef private

#include "frame_scheduler_v2.hpp"

class MutexPIPTest : public ::testing::Test {
protected:
    void SetUp() override {
        Scheduler::instance().init();
        Scheduler::instance().create_task([](){}, nullptr, 0, TaskPriority::Idle);
        FrameSchedulerV2::instance().notify_render_complete();
        Scheduler::instance().set_started(true);
    }
};

// 1. Basic Priority Inheritance Protocol (PIP)
TEST_F(MutexPIPTest, BasicPIP) {
    Mutex m;
    
    // Low task takes the lock
    TaskControlBlock* task_low = Scheduler::instance().create_task([](){}, nullptr, 0, TaskPriority::Low);
    Scheduler::instance().schedule();
    m.lock();
    EXPECT_EQ(task_low->current_priority, TaskPriority::Low);
    
    // Suspend Low
    Scheduler::instance().set_task_state(task_low->id, TaskState::Suspended);
    
    // High task tries to take the lock
    TaskControlBlock* task_high = Scheduler::instance().create_task([](){}, nullptr, 0, TaskPriority::High);
    Scheduler::instance().schedule();
    
    // Manually simulate lock() transient state since lock() would block/spin
    m.wait_mask_ |= (1 << task_high->id);
    task_high->waiting_on_mutex = &m;
    
    // Call propagate_priority manually to simulate what lock() does before spinning
    Mutex::propagate_priority(task_high);
    
    // Low should now be boosted to High
    EXPECT_EQ(task_low->current_priority, TaskPriority::High);
    
    // Now simulate timeout / cancellation
    m.wait_mask_ &= ~(1 << task_high->id);
    task_high->waiting_on_mutex = nullptr;
    Mutex::recalculate_priority_chain(task_low);
    
    // Low should drop back to Low
    EXPECT_EQ(task_low->current_priority, TaskPriority::Low);
}

// 2. Transitive PIP
TEST_F(MutexPIPTest, TransitiveInheritance) {
    Mutex m1, m2;
    
    // Low task takes m1
    TaskControlBlock* task_low = Scheduler::instance().create_task([](){}, nullptr, 0, TaskPriority::Low);
    Scheduler::instance().schedule();
    m1.lock();
    
    Scheduler::instance().set_task_state(task_low->id, TaskState::Suspended);
    
    // Med task takes m2
    TaskControlBlock* task_med = Scheduler::instance().create_task([](){}, nullptr, 0, TaskPriority::Normal);
    Scheduler::instance().schedule();
    m2.lock();
    
    Scheduler::instance().set_task_state(task_med->id, TaskState::Suspended);
    
    // Simulate Med waiting on m1
    m1.wait_mask_ |= (1 << task_med->id);
    task_med->waiting_on_mutex = &m1;
    Mutex::propagate_priority(task_med);
    
    // Low is boosted to Normal
    EXPECT_EQ(task_low->current_priority, TaskPriority::Normal);
    
    // High task tries to take m2
    TaskControlBlock* task_high = Scheduler::instance().create_task([](){}, nullptr, 0, TaskPriority::High);
    Scheduler::instance().schedule();
    
    // Simulate High waiting on m2
    m2.wait_mask_ |= (1 << task_high->id);
    task_high->waiting_on_mutex = &m2;
    Mutex::propagate_priority(task_high);
    
    // Med is boosted to High, AND Low is transitively boosted to High!
    EXPECT_EQ(task_med->current_priority, TaskPriority::High);
    EXPECT_EQ(task_low->current_priority, TaskPriority::High);
    
    // Simulate High timing out
    m2.wait_mask_ &= ~(1 << task_high->id);
    task_high->waiting_on_mutex = nullptr;
    Mutex::recalculate_priority_chain(task_med);
    
    // Med should revert to Normal, and transitively revert Low to Normal!
    EXPECT_EQ(task_med->current_priority, TaskPriority::Normal);
    EXPECT_EQ(task_low->current_priority, TaskPriority::Normal);
    
    // Simulate Med getting m1 (Low unlocks m1). First we must switch to Low.
    Scheduler::instance().set_task_state(task_high->id, TaskState::Suspended);
    Scheduler::instance().set_task_state(task_med->id, TaskState::Suspended);
    Scheduler::instance().set_task_state(task_low->id, TaskState::Ready);
    Scheduler::instance().schedule();
    
    m1.unlock();
    
    // Since Low unlocked m1, it should revert to Low
    EXPECT_EQ(task_low->current_priority, TaskPriority::Low);
}
