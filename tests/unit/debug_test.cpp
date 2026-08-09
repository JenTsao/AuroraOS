#include <gtest/gtest.h>
#include "task.hpp"

TEST(DebugTest, CheckTasks) {
    Scheduler::instance().init();
    TaskControlBlock* t0 = Scheduler::instance().create_task([](){}, nullptr, 0, TaskPriority::Idle);
    TaskControlBlock* t1 = Scheduler::instance().create_task([](){}, nullptr, 0, TaskPriority::Normal);
    TaskControlBlock* t2 = Scheduler::instance().create_task([](){}, nullptr, 0, TaskPriority::Normal);
    ASSERT_NE(t0, nullptr);
    ASSERT_NE(t1, nullptr);
    ASSERT_NE(t2, nullptr);
    printf("t0=%p (id=%u), t1=%p (id=%u), t2=%p (id=%u)\n", t0, t0->id, t1, t1->id, t2, t2->id);
    
    Scheduler::instance().set_started(true);
    Scheduler::instance().schedule();
    
    TaskControlBlock* current = Scheduler::instance().get_current_tcb();
    ASSERT_NE(current, nullptr) << "get_current_tcb() returned nullptr! task_count=" 
                                << Scheduler::instance().get_task_count();
                                
    printf("current after schedule: %p (id=%u)\n", current, current->id);
}
