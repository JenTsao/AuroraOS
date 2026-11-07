#include "semaphore.hpp"
#include "../interrupt/timer.hpp"

bool Semaphore::wait(uint32_t timeout_ticks) {
    TaskControlBlock* current = Scheduler::instance().get_current_tcb();

    {
        IrqGuard guard;
        if (count_ > 0) {
            count_--;
            if (current && current->scheduler.waiting_queue == &wait_queue_) {
                wait_queue_.remove(current);
            }
            return true;
        }
    }

    if (!current)
        return false;

    uint32_t start_tick = TimerManager::instance().get_current_tick();

    while (true) {
        {
            IrqGuard guard;
            if (count_ > 0) {
                count_--;
                wait_queue_.remove(current);
                return true;
            }

            uint32_t elapsed = TimerManager::instance().get_current_tick() - start_tick;
            if (timeout_ticks != 0xFFFFFFFF && elapsed >= timeout_ticks) {
                wait_queue_.remove(current);
                return false;
            }

            if (current->scheduler.waiting_queue != &wait_queue_) {
                wait_queue_.enqueue(current);
            }
            if (timeout_ticks != 0xFFFFFFFF) {
                current->scheduler.sleep_ticks = timeout_ticks - elapsed;
                Scheduler::instance().set_task_state(current->scheduler.id, TaskState::Sleeping);
            } else {
                Scheduler::instance().set_task_state(current->scheduler.id, TaskState::Suspended);
            }
        }

        Scheduler::instance().schedule();
    }
}

bool Semaphore::try_wait() {
    IrqGuard guard;
    if (count_ > 0) {
        count_--;
        return true;
    }
    return false;
}

void Semaphore::signal(bool in_isr) {
    bool trigger_reschedule = false;
    {
        IrqGuard guard;
        count_++;
        while (!wait_queue_.empty()) {
            TaskControlBlock* t = wait_queue_.dequeue();
            if (t && (t->scheduler.state == TaskState::Suspended || t->scheduler.state == TaskState::Sleeping)) {
                t->scheduler.sleep_ticks = 0;
                Scheduler::instance().set_task_state(t->scheduler.id, TaskState::Ready);
                trigger_reschedule = true;
                break;
            }
        }
    }

    // 中断上下文不直接调度：在中断里执行完整的 schedule() 会把被打断的
    // 任务误当"当前任务"做时间片轮转，并可能提前重开中断破坏外层临界区。
    // 唤醒已置 Ready，由 SysTick 尾部 / PendSV 悬挂统一完成抢占。
    if (trigger_reschedule && !in_isr) {
        Scheduler::instance().schedule();
    }
}
