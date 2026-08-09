#ifndef AURORA_TASK_NOTIFY_HPP
#define AURORA_TASK_NOTIFY_HPP

#include "task.hpp"

class TaskNotify {
public:
    // 1. 发送通知 (极其适合在 SysTick 或网卡硬件中断 ISR 中极速调用！)
    static bool give(uint32_t target_task_id, uint32_t val = 1, bool yield = true) {
        IrqGuard guard;
        TaskControlBlock* target = Scheduler::instance().get_task_by_id(target_task_id);
        if (!target) {
            return false;
        }

        // 直接向 TCB 写入通知值，标记状态
        target->notify_value |= val;
        target->notify_pending = true;

        // 如果目标线程正处于等待通知的挂起状态，瞬间将它唤醒，拔高为 Ready！
        if (target->state == TaskState::Blocked_On_Notify) {
            Scheduler::instance().set_task_state(target->id, TaskState::Ready);
            
            if (yield) {
                // 触发立即抢占调度
                Scheduler::instance().schedule();
            }
            return true;
        }

        return true;
    }

    // 2. 接收通知 (消费者调用)
    static uint32_t take(bool clear_on_exit = true) {
        TaskControlBlock* current = Scheduler::instance().get_current_tcb();

        while (true) {
            {
                IrqGuard guard;
                if (current->notify_pending) {
                    uint32_t val = current->notify_value;
                    if (clear_on_exit) {
                        current->notify_value = 0;
                        current->notify_pending = false;
                    }
                    return val; // 成功拿到 32 位通知值
                }

                // 没有收到通知，挂起当前任务自身，让出 CPU
                Scheduler::instance().set_task_state(current->id, TaskState::Blocked_On_Notify);
            }

            Scheduler::instance().schedule();
        }
    }
};

#endif
