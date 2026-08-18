#include "semaphore.hpp"
#include "../interrupt/timer.hpp"

bool Semaphore::wait(uint32_t timeout_ticks) {
    TaskControlBlock* current = Scheduler::instance().get_current_tcb();
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

void Semaphore::signal() {
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
                            best_prio = prio;
                            best_id = i;
                        }
                    } else {
                        wait_mask_ &= ~(1 << i);
                    }
                }
            }
            if (best_id != 0xFFFFFFFF) {
                wait_mask_ &= ~(1 << best_id);
                Scheduler::instance().set_task_state(best_id, TaskState::Ready);
                trigger_reschedule = true;
            }
        }
    }

    if (trigger_reschedule) {
        Scheduler::instance().schedule();
    }
}
