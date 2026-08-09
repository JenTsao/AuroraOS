#include <gtest/gtest.h>
#include <stdint.h>
#include "task.hpp"
#include "memory.hpp"

static uint8_t mock_heap[8192];
static uint32_t boot_test_stack[256];

static void boot_task_entry() {
    while(1) {
        Scheduler::instance().sleep_ms(100);
    }
}

TEST(SystemBootTest, CanInitializeAndRunTask) {
    // 1. Init heap
    KernelHeap::instance().init(&mock_heap[0], &mock_heap[sizeof(mock_heap)]);
    
    // 2. Init scheduler
    Scheduler::instance().init();
    
    // 3. Create a task
    TaskControlBlock* tcb = Scheduler::instance().create_task(
        boot_task_entry,
        boot_test_stack,
        sizeof(boot_test_stack),
        TaskPriority::Normal
    );
    
    ASSERT_NE(tcb, nullptr);
    EXPECT_EQ(tcb->state, TaskState::Ready);
    
    // 4. Tick scheduler manually to simulate boot and timer tick
    Scheduler::instance().tick_update();
    Scheduler::instance().schedule();
}
