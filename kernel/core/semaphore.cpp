#include "semaphore.hpp"
#include "../interrupt/timer.hpp"

bool Semaphore::wait(uint32_t timeout_ticks) {
    TaskControlBlock* current = Scheduler::instance().get_current_tcb();

    {
        IrqGuard guard;
        if (count_ > 0) {
            count_--;
            if (current) {
                wait_mask_ &= ~(1 << current->scheduler.id);
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
                wait_mask_ &= ~(1 << current->scheduler.id);
                return true;
            }

            uint32_t elapsed = TimerManager::instance().get_current_tick() - start_tick;
            if (timeout_ticks != 0xFFFFFFFF && elapsed >= timeout_ticks) {
                wait_mask_ &= ~(1 << current->scheduler.id);
                return false;
            }

            wait_mask_ |= (1 << current->scheduler.id);
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
        if (wait_mask_ != 0) {
            uint32_t best_id = 0xFFFFFFFF;
            uint8_t best_prio = 0;
            for (int i = 0; i < Scheduler::get_max_tasks(); i++) {
                if (wait_mask_ & (1U << i)) {
                    TaskControlBlock* t = Scheduler::instance().get_task_by_id(i);
                    if (t && (t->scheduler.state == TaskState::Suspended || t->scheduler.state == TaskState::Sleeping)) {
                        uint8_t prio = static_cast<uint8_t>(t->scheduler.current_priority);
                        if (best_id == 0xFFFFFFFF || prio > best_prio) {
                            best_id = i;
                            best_prio = prio;
                        }
                    }
                }
            }

            if (best_id != 0xFFFFFFFF) {
                Scheduler::instance().set_task_state(best_id, TaskState::Ready);
                trigger_reschedule = true;
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
