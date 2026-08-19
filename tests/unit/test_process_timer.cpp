#include <gtest/gtest.h>

#define private public
#include "task/task.hpp"
#include "core/process_timer.hpp"
#include "interrupt/timer.hpp"
#include "core/syscall_dispatcher.hpp"
#undef private

using namespace auroraos::kernel;

class ProcessTimerTest : public ::testing::Test {
protected:
    void SetUp() override {
        Scheduler::instance().init();
        ProcessTimerManager::instance().init();
        // Create idle task
        Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Idle);
        Scheduler::instance().set_started(true);
    }
};

// 1. 单次相对定时器触发信号测试
TEST_F(ProcessTimerTest, OneShotRelativeSignal) {
    TaskControlBlock* task = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Normal);
    Scheduler::instance().schedule();

    static bool signal_received = false;
    signal_received = false;
    task->security.sig_actions[14].sa_handler = [](int sig) {
        if (sig == 14) signal_received = true;
    };

    ProcessTimerDesc desc{};
    desc.flags = TimerFlags::OneShot | TimerFlags::Relative | TimerFlags::NotifySignal;
    desc.initial_delay_ms = 10;
    desc.interval_ms = 0;
    desc.notify_param = 14; // SIGALRM

    int timer_id = ProcessTimerManager::instance().create_timer(task, &desc);
    EXPECT_GE(timer_id, 0);

    // 运行 9 个 tick，不应触发
    for (int i = 0; i < 9; i++) {
        TimerManager::instance().on_tick();
    }
    EXPECT_FALSE(signal_received);

    // 第 10 个 tick，定时器到期并发送信号
    TimerManager::instance().on_tick();
    EXPECT_TRUE(task->security.pending_signals & (1U << 14));

    // 调度执行信号分发
    Scheduler::instance().signal_dispatch(task);
    EXPECT_TRUE(signal_received);

    // 检查定时器状态：单次定时器触发后应自动置为非激活
    const ProcessTimer* pt = ProcessTimerManager::instance().get_timer(timer_id);
    ASSERT_NE(pt, nullptr);
    EXPECT_FALSE(pt->active);
}

// 2. 周期定时器与无时钟漂移测试
TEST_F(ProcessTimerTest, PeriodicTimerNoDrift) {
    TaskControlBlock* task = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Normal);
    Scheduler::instance().schedule();

    static int fire_count = 0;
    fire_count = 0;
    task->security.sig_actions[14].sa_handler = [](int) {
        fire_count++;
    };

    ProcessTimerDesc desc{};
    desc.flags = TimerFlags::Periodic | TimerFlags::Relative | TimerFlags::NotifySignal;
    desc.initial_delay_ms = 5;
    desc.interval_ms = 5;
    desc.notify_param = 14;

    int timer_id = ProcessTimerManager::instance().create_timer(task, &desc);
    EXPECT_GE(timer_id, 0);

    // 运行 15 个 tick，应该精准触发 3 次
    for (int i = 1; i <= 15; i++) {
        TimerManager::instance().on_tick();
        if (task->security.pending_signals & (1U << 14)) {
            Scheduler::instance().signal_dispatch(task);
        }
    }
    EXPECT_EQ(fire_count, 3);

    // 定时器仍应处于活跃状态
    const ProcessTimer* pt = ProcessTimerManager::instance().get_timer(timer_id);
    ASSERT_NE(pt, nullptr);
    EXPECT_TRUE(pt->active);
}

// 3. 绝对时间戳定时器测试
TEST_F(ProcessTimerTest, AbsoluteTimestampTimer) {
    TaskControlBlock* task = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Normal);
    Scheduler::instance().schedule();

    // 先前进一些 tick
    for (int i = 0; i < 20; i++) {
        TimerManager::instance().on_tick();
    }
    uint32_t now = TimerManager::instance().get_current_tick();

    ProcessTimerDesc desc{};
    desc.flags = TimerFlags::OneShot | TimerFlags::Absolute | TimerFlags::NotifySignal;
    desc.initial_delay_ms = now + 10; // 绝对 tick
    desc.interval_ms = 0;
    desc.notify_param = 14;

    int timer_id = ProcessTimerManager::instance().create_timer(task, &desc);
    EXPECT_GE(timer_id, 0);

    // 前进 9 个 tick
    for (int i = 0; i < 9; i++) {
        TimerManager::instance().on_tick();
    }
    EXPECT_FALSE(task->security.pending_signals & (1U << 14));

    // 到达绝对时间
    TimerManager::instance().on_tick();
    EXPECT_TRUE(task->security.pending_signals & (1U << 14));
}

// 4. 事件与 IPC 通知模式测试 (NotifyEvent / NotifyIpc)
TEST_F(ProcessTimerTest, NotifyEventAndIpc) {
    TaskControlBlock* task = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Normal);
    Scheduler::instance().schedule();

    // 任务进入挂起休眠状态
    Scheduler::instance().set_task_state(task->scheduler.id, TaskState::Suspended);
    EXPECT_EQ(task->scheduler.state, TaskState::Suspended);

    ProcessTimerDesc desc{};
    desc.flags = TimerFlags::OneShot | TimerFlags::Relative | TimerFlags::NotifyEvent;
    desc.initial_delay_ms = 5;
    desc.interval_ms = 0;
    desc.notify_param = 0x88; // 88 掩码

    int timer_id = ProcessTimerManager::instance().create_timer(task, &desc);
    EXPECT_GE(timer_id, 0);

    for (int i = 0; i < 5; i++) {
        TimerManager::instance().on_tick();
    }

    // 定时器到期后，任务应被唤醒并置为 Ready，且 notify_value 包含指定掩码
    EXPECT_EQ(task->scheduler.state, TaskState::Ready);
    EXPECT_TRUE(task->ipc.notify_pending);
    EXPECT_EQ(task->ipc.notify_value, 0x88);
}

// 5. 剩余时间查询 (get_time)
TEST_F(ProcessTimerTest, GetTimeRemaining) {
    TaskControlBlock* task = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Normal);
    Scheduler::instance().schedule();

    ProcessTimerDesc desc{};
    desc.flags = TimerFlags::OneShot | TimerFlags::Relative | TimerFlags::NotifySignal;
    desc.initial_delay_ms = 50;
    desc.interval_ms = 0;
    desc.notify_param = 14;

    int timer_id = ProcessTimerManager::instance().create_timer(task, &desc);
    EXPECT_GE(timer_id, 0);

    uint32_t remaining = 0;
    EXPECT_EQ(ProcessTimerManager::instance().get_time(task, timer_id, &remaining), 0);
    EXPECT_EQ(remaining, 50);

    // 运行 20ms
    for (int i = 0; i < 20; i++) {
        TimerManager::instance().on_tick();
    }

    EXPECT_EQ(ProcessTimerManager::instance().get_time(task, timer_id, &remaining), 0);
    EXPECT_EQ(remaining, 30);
}

// 6. 停止与删除定时器
TEST_F(ProcessTimerTest, StopAndDeleteTimer) {
    TaskControlBlock* task = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Normal);
    Scheduler::instance().schedule();

    ProcessTimerDesc desc{};
    desc.flags = TimerFlags::OneShot | TimerFlags::Relative | TimerFlags::NotifySignal;
    desc.initial_delay_ms = 10;
    desc.interval_ms = 0;
    desc.notify_param = 14;

    int timer_id = ProcessTimerManager::instance().create_timer(task, &desc);
    EXPECT_GE(timer_id, 0);

    // 停止定时器
    EXPECT_EQ(ProcessTimerManager::instance().stop_timer(task, timer_id), 0);

    // 走 20 个 tick，定时器不应触发
    for (int i = 0; i < 20; i++) {
        TimerManager::instance().on_tick();
    }
    EXPECT_FALSE(task->security.pending_signals & (1U << 14));

    // 删除定时器
    EXPECT_EQ(ProcessTimerManager::instance().delete_timer(task, timer_id), 0);
    const ProcessTimer* pt = ProcessTimerManager::instance().get_timer(timer_id);
    ASSERT_NE(pt, nullptr);
    EXPECT_FALSE(pt->allocated);
}

// 7. 任务终止时自动回收定时器资源
TEST_F(ProcessTimerTest, CleanupOnTaskExit) {
    TaskControlBlock* task = Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Normal);
    Scheduler::instance().schedule();

    ProcessTimerDesc desc{};
    desc.flags = TimerFlags::Periodic | TimerFlags::Relative | TimerFlags::NotifySignal;
    desc.initial_delay_ms = 10;
    desc.interval_ms = 10;
    desc.notify_param = 14;

    int timer_id = ProcessTimerManager::instance().create_timer(task, &desc);
    EXPECT_GE(timer_id, 0);

    const ProcessTimer* pt = ProcessTimerManager::instance().get_timer(timer_id);
    ASSERT_NE(pt, nullptr);
    EXPECT_TRUE(pt->allocated);

    // 回收任务定时器
    ProcessTimerManager::instance().cleanup_task_timers(task->scheduler.id);
    EXPECT_FALSE(pt->allocated);
}
