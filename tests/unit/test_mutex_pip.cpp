#include <gtest/gtest.h>

#define private public
#include "mutex.hpp"
#include "semaphore.hpp"
#include "posix.hpp"
#undef private

#include "frame_scheduler_v2.hpp"

class MutexPIPTest : public ::testing::Test {
protected:
    void SetUp() override {
        Scheduler::instance().init();
        Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Idle);
        FrameSchedulerV2::instance().notify_render_complete();
        Scheduler::instance().set_started(true);
    }
};

// 1. Basic Priority Inheritance Protocol (PIP)
TEST_F(MutexPIPTest, BasicPIP) {
    Mutex m;

    // Low task takes the lock
    TaskControlBlock* task_low = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Low);
    Scheduler::instance().schedule();
    m.lock();
    EXPECT_EQ(task_low->scheduler.current_priority, TaskPriority::Low);

    // Suspend Low
    Scheduler::instance().set_task_state(task_low->scheduler.id, TaskState::Suspended);

    // High task tries to take the lock
    TaskControlBlock* task_high = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::High);
    Scheduler::instance().schedule();

    // Manually simulate lock() transient state since lock() would block/spin
    m.wait_mask_ |= (1 << task_high->scheduler.id);
    task_high->scheduler.waiting_on_mutex = &m;

    // Call propagate_priority manually to simulate what lock() does before spinning
    Mutex::propagate_priority(task_high);

    // Low should now be boosted to High
    EXPECT_EQ(task_low->scheduler.current_priority, TaskPriority::High);

    // Now simulate timeout / cancellation
    m.wait_mask_ &= ~(1 << task_high->scheduler.id);
    task_high->scheduler.waiting_on_mutex = nullptr;
    Mutex::recalculate_priority_chain(task_low);

    // Low should drop back to Low
    EXPECT_EQ(task_low->scheduler.current_priority, TaskPriority::Low);
}

// 2. Transitive PIP
TEST_F(MutexPIPTest, TransitiveInheritance) {
    Mutex m1, m2;

    // Low task takes m1
    TaskControlBlock* task_low = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Low);
    Scheduler::instance().schedule();
    m1.lock();

    Scheduler::instance().set_task_state(task_low->scheduler.id, TaskState::Suspended);

    // Med task takes m2
    TaskControlBlock* task_med = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Normal);
    Scheduler::instance().schedule();
    m2.lock();

    Scheduler::instance().set_task_state(task_med->scheduler.id, TaskState::Suspended);

    // Simulate Med waiting on m1
    m1.wait_mask_ |= (1 << task_med->scheduler.id);
    task_med->scheduler.waiting_on_mutex = &m1;
    Mutex::propagate_priority(task_med);

    // Low is boosted to Normal
    EXPECT_EQ(task_low->scheduler.current_priority, TaskPriority::Normal);

    // High task tries to take m2
    TaskControlBlock* task_high = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::High);
    Scheduler::instance().schedule();

    // Simulate High waiting on m2
    m2.wait_mask_ |= (1 << task_high->scheduler.id);
    task_high->scheduler.waiting_on_mutex = &m2;
    Mutex::propagate_priority(task_high);

    // Med is boosted to High, AND Low is transitively boosted to High!
    EXPECT_EQ(task_med->scheduler.current_priority, TaskPriority::High);
    EXPECT_EQ(task_low->scheduler.current_priority, TaskPriority::High);

    // Simulate High timing out
    m2.wait_mask_ &= ~(1 << task_high->scheduler.id);
    task_high->scheduler.waiting_on_mutex = nullptr;
    Mutex::recalculate_priority_chain(task_med);

    // Med should revert to Normal, and transitively revert Low to Normal!
    EXPECT_EQ(task_med->scheduler.current_priority, TaskPriority::Normal);
    EXPECT_EQ(task_low->scheduler.current_priority, TaskPriority::Normal);

    // Simulate Med getting m1 (Low unlocks m1). First we must switch to Low.
    Scheduler::instance().set_task_state(task_high->scheduler.id, TaskState::Suspended);
    Scheduler::instance().set_task_state(task_med->scheduler.id, TaskState::Suspended);
    Scheduler::instance().set_task_state(task_low->scheduler.id, TaskState::Ready);
    Scheduler::instance().schedule();

    m1.unlock();

    // Since Low unlocked m1, it should revert to Low
    EXPECT_EQ(task_low->scheduler.current_priority, TaskPriority::Low);
}

// 3. Immediate Priority Ceiling Protocol (IPCP)
TEST_F(MutexPIPTest, ImmediatePriorityCeiling) {
    // 构造一个天花板优先级为 High 的互斥锁
    Mutex ceiling_mutex(TaskPriority::High);

    // 创建一个 Normal 优先级的任务
    TaskControlBlock* task = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Normal);
    Scheduler::instance().schedule();

    EXPECT_EQ(task->scheduler.current_priority, TaskPriority::Normal);

    // 获取锁：任务立即提权到天花板优先级 High
    bool acquired = ceiling_mutex.lock();
    EXPECT_TRUE(acquired);
    EXPECT_EQ(task->scheduler.current_priority, TaskPriority::High);

    // 释放锁：任务优先级自动恢复为基础优先级 Normal
    ceiling_mutex.unlock();
    EXPECT_EQ(task->scheduler.current_priority, TaskPriority::Normal);
}

// 4. 跨任务死锁循环等待检测 (Deadlock Detection)
TEST_F(MutexPIPTest, DeadlockDetectionAvoidance) {
    Mutex ma, mb;

    TaskControlBlock* t1 = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Normal);
    Scheduler::instance().schedule();
    // T1 获取 MA
    EXPECT_TRUE(ma.lock());

    Scheduler::instance().set_task_state(t1->scheduler.id, TaskState::Suspended);

    TaskControlBlock* t2 = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Normal);
    Scheduler::instance().schedule();
    // T2 获取 MB
    EXPECT_TRUE(mb.lock());

    // 模拟 T1 等待 MB
    mb.wait_mask_ |= (1 << t1->scheduler.id);
    t1->scheduler.waiting_on_mutex = &mb;

    // 此时 T2 尝试获取 MA：由于 T2 拥有 MB，而 T1 正在等待 MB 且拥有 MA，
    // 若 T2 等待 MA 将构成 ABBA 死锁闭环 (T1 -> MB -> T2 -> MA -> T1)！
    // 内核死锁检测机制应立即识别闭环并拒绝 T2 的锁请求，返回 false 并设置 errno 为 EDEADLK (35)
    bool t2_ma_locked = ma.lock();
    EXPECT_FALSE(t2_ma_locked);
    EXPECT_EQ(t2->task.errno_val, 35); // EDEADLK
}

// 5. 信号量阻塞与定时等待测试
TEST_F(MutexPIPTest, SemaphoreTimedWaitAndSignal) {
    Semaphore sem(0);

    TaskControlBlock* task = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Normal);
    Scheduler::instance().schedule();
    (void)task;

    // try_wait 应该立即失败
    EXPECT_FALSE(sem.try_wait());

    // 生产者 post 增加资源
    sem.signal();
    EXPECT_EQ(sem.get_count(), 1);

    // try_wait 应该成功
    EXPECT_TRUE(sem.try_wait());
    EXPECT_EQ(sem.get_count(), 0);
}

// 6. POSIX sem_* 零堆分配池测试
TEST_F(MutexPIPTest, PosixSemPoolAllocation) {
    sem_t s1 = nullptr;
    sem_t s2 = nullptr;

    EXPECT_EQ(sem_init(&s1, 0, 1), 0);
    EXPECT_NE(s1, nullptr);

    EXPECT_EQ(sem_init(&s2, 0, 0), 0);
    EXPECT_NE(s2, nullptr);

    // s1 初始为 1，trywait 应成功
    EXPECT_EQ(sem_trywait(&s1), 0);
    EXPECT_EQ(sem_trywait(&s1), -1); // 耗尽

    // post s1
    EXPECT_EQ(sem_post(&s1), 0);
    EXPECT_EQ(sem_trywait(&s1), 0);

    // destroy
    EXPECT_EQ(sem_destroy(&s1), 0);
    EXPECT_EQ(sem_destroy(&s2), 0);
}
