#include <gtest/gtest.h>

#define private public
#include "mutex.hpp"
#include "semaphore.hpp"
#include "posix.hpp"
#undef private

#include "frame_scheduler_v2.hpp"

using auroraos::kernel::WaitPolicy;
using auroraos::kernel::WaitQueue;

class MutexPIPTest : public ::testing::Test {
protected:
    void SetUp() override {
        Scheduler::instance().init();
        Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Idle);
        FrameSchedulerV2::instance().notify_frame_complete();
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
    m.wait_queue_.enqueue(task_high);
    task_high->scheduler.waiting_on_mutex = &m;
    Scheduler::instance().set_task_state(task_high->scheduler.id, TaskState::Suspended);

    // Call propagate_priority manually to simulate what lock() does before spinning
    Mutex::propagate_priority(task_high);

    // Low should now be boosted to High
    EXPECT_EQ(task_low->scheduler.current_priority, TaskPriority::High);

    // Now simulate timeout / cancellation
    m.wait_queue_.remove(task_high);
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
    m1.wait_queue_.enqueue(task_med);
    task_med->scheduler.waiting_on_mutex = &m1;
    Mutex::propagate_priority(task_med);

    // Low is boosted to Normal
    EXPECT_EQ(task_low->scheduler.current_priority, TaskPriority::Normal);

    // High task tries to take m2
    TaskControlBlock* task_high = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::High);
    Scheduler::instance().schedule();

    // Simulate High waiting on m2
    m2.wait_queue_.enqueue(task_high);
    task_high->scheduler.waiting_on_mutex = &m2;
    Scheduler::instance().set_task_state(task_high->scheduler.id, TaskState::Suspended);
    Mutex::propagate_priority(task_high);

    // Med is boosted to High, AND Low is transitively boosted to High!
    EXPECT_EQ(task_med->scheduler.current_priority, TaskPriority::High);
    EXPECT_EQ(task_low->scheduler.current_priority, TaskPriority::High);

    // Simulate High timing out
    m2.wait_queue_.remove(task_high);
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
    mb.wait_queue_.enqueue(t1);
    t1->scheduler.waiting_on_mutex = &mb;

    // 此时 T2 尝试获取 MA：由于 T2 拥有 MB，而 T1 正在等待 MB 且拥有 MA，
    // 若 T2 等待 MA 将构成 ABBA 死锁闭环 (T1 -> MB -> T2 -> MA -> T1)！
    // 内核死锁检测机制应立即识别闭环并拒绝 T2 的锁请求，返回 false 并设置 errno 为 EDEADLK (35)
    bool t2_ma_locked = ma.lock();
    EXPECT_FALSE(t2_ma_locked);
    EXPECT_EQ(t2->errno_val, 35); // EDEADLK
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

// 7. 统一 WaitQueue：FIFO 与 Priority 两种入队策略 + 同级 FIFO 公平性测试
TEST_F(MutexPIPTest, UnifiedWaitQueueFifoAndPriorityPolicies) {
    TaskControlBlock* t_low = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Low);
    TaskControlBlock* t_high = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::High);
    TaskControlBlock* t_norm1 = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Normal);
    TaskControlBlock* t_norm2 = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Normal);

    // (a) FIFO 策略：严格按入队顺序出队，忽略优先级高低
    WaitQueue fifo_q(WaitPolicy::Fifo);
    EXPECT_EQ(fifo_q.policy(), WaitPolicy::Fifo);
    fifo_q.enqueue(t_low);
    fifo_q.enqueue(t_high);
    fifo_q.enqueue(t_norm1);
    EXPECT_EQ(fifo_q.size(), 3u);
    EXPECT_EQ(fifo_q.highest_priority(), static_cast<uint8_t>(TaskPriority::High));

    EXPECT_EQ(fifo_q.dequeue(), t_low);
    EXPECT_EQ(fifo_q.dequeue(), t_high);
    EXPECT_EQ(fifo_q.dequeue(), t_norm1);
    EXPECT_TRUE(fifo_q.empty());

    // (b) Priority 策略：按优先级降序，同优先级维持入队先后顺序（即使后入队的任务 ID 更小）
    WaitQueue prio_q(WaitPolicy::Priority);
    prio_q.enqueue(t_norm2); // 先入队，ID 较大
    prio_q.enqueue(t_low);
    prio_q.enqueue(t_norm1); // 后入队，同为 Normal，但 ID 较小
    prio_q.enqueue(t_high);

    EXPECT_EQ(prio_q.dequeue(), t_high);
    EXPECT_EQ(prio_q.dequeue(), t_norm2);
    EXPECT_EQ(prio_q.dequeue(), t_norm1);
    EXPECT_EQ(prio_q.dequeue(), t_low);
    EXPECT_TRUE(prio_q.empty());
}

// 8. 传递性 PIP 动态改写已入队等待者优先级时，WaitQueue 自动重排消除优先级反转
TEST_F(MutexPIPTest, TransitivePIPReordersWaitQueueAndPreventsInversion) {
    Mutex m1, m2;

    // t_owner 持有 m1
    TaskControlBlock* t_owner = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Low);
    Scheduler::instance().schedule();
    ASSERT_TRUE(m1.lock());
    Scheduler::instance().set_task_state(t_owner->scheduler.id, TaskState::Suspended);

    // t_med (Low) 持有 m2
    TaskControlBlock* t_med = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Low);
    Scheduler::instance().schedule();
    ASSERT_TRUE(m2.lock());
    Scheduler::instance().set_task_state(t_med->scheduler.id, TaskState::Suspended);

    // t_norm (Normal) 与 t_med (Low) 先后在 m1 上阻塞等待：
    // 初始队列顺序应为 [t_norm (Normal), t_med (Low)]
    TaskControlBlock* t_norm = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Normal);
    Scheduler::instance().set_task_state(t_norm->scheduler.id, TaskState::Suspended);
    t_norm->scheduler.waiting_on_mutex = &m1;
    m1.wait_queue_.enqueue(t_norm);
    Mutex::propagate_priority(t_norm);

    t_med->scheduler.waiting_on_mutex = &m1;
    m1.wait_queue_.enqueue(t_med);
    Mutex::propagate_priority(t_med);

    EXPECT_EQ(m1.wait_queue_.head(), t_norm);

    // 现在 t_high (High) 阻塞在 t_med 持有的 m2 上：
    // 传递性 PIP 将 t_med 提权至 High，Scheduler::set_task_priority 自动触发
    // m1.wait_queue_.on_priority_changed(t_med)，把 t_med 重排到 t_norm 之前！
    TaskControlBlock* t_high = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::High);
    Scheduler::instance().set_task_state(t_high->scheduler.id, TaskState::Suspended);
    t_high->scheduler.waiting_on_mutex = &m2;
    m2.wait_queue_.enqueue(t_high);
    Mutex::propagate_priority(t_high);

    EXPECT_EQ(t_med->scheduler.current_priority, TaskPriority::High);
    EXPECT_EQ(t_owner->scheduler.current_priority, TaskPriority::High);
    EXPECT_EQ(m1.wait_queue_.head(), t_med);

    // 当 t_owner 释放 m1 时，必须优先唤醒已被提权到 High 的 t_med，而非 Normal 的 t_norm！
    Scheduler::instance().set_task_state(t_owner->scheduler.id, TaskState::Ready);
    Scheduler::instance().schedule();
    m1.unlock();

    EXPECT_EQ(t_med->scheduler.state, TaskState::Ready);
    EXPECT_EQ(t_norm->scheduler.state, TaskState::Suspended);
}

// 9. Semaphore 支持 Priority / FIFO 两种等待策略及任务终止自动清理
TEST_F(MutexPIPTest, SemaphoreWaitPoliciesAndTerminationCleanup) {
    TaskControlBlock* t_low = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Low);
    TaskControlBlock* t_high = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::High);
    TaskControlBlock* t_norm = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Normal);

    // (a) FIFO 信号量：按入队先后唤醒
    Semaphore fifo_sem(0, WaitPolicy::Fifo);
    Scheduler::instance().set_task_state(t_low->scheduler.id, TaskState::Suspended);
    Scheduler::instance().set_task_state(t_high->scheduler.id, TaskState::Suspended);
    fifo_sem.wait_queue().enqueue(t_low);
    fifo_sem.wait_queue().enqueue(t_high);

    fifo_sem.signal(/*in_isr=*/true);
    EXPECT_EQ(t_low->scheduler.state, TaskState::Ready);
    EXPECT_EQ(t_high->scheduler.state, TaskState::Suspended);

    fifo_sem.signal(/*in_isr=*/true);
    EXPECT_EQ(t_high->scheduler.state, TaskState::Ready);

    // (b) 默认 Priority 信号量 + 等待任务被终止时的自动解链
    Semaphore prio_sem(0, WaitPolicy::Priority);
    Scheduler::instance().set_task_state(t_norm->scheduler.id, TaskState::Suspended);
    Scheduler::instance().set_task_state(t_high->scheduler.id, TaskState::Suspended);
    prio_sem.wait_queue().enqueue(t_norm);
    prio_sem.wait_queue().enqueue(t_high);
    EXPECT_EQ(prio_sem.wait_queue().head(), t_high);

    // 终止排在队首的 t_high：release_task_resources 应将其从 prio_sem.wait_queue_ 安全摘除
    Scheduler::instance().terminate_task(t_high->scheduler.id);
    EXPECT_FALSE(prio_sem.wait_queue().contains(t_high));
    EXPECT_EQ(prio_sem.wait_queue().head(), t_norm);

    // 此时 signal 应直接唤醒剩余的 t_norm
    prio_sem.signal(/*in_isr=*/true);
    EXPECT_EQ(t_norm->scheduler.state, TaskState::Ready);
}
