#ifndef SEMAPHORE_HPP
#define SEMAPHORE_HPP

#include "task.hpp"

class Semaphore {
private:
    int count_;
    uint32_t wait_mask_ = 0;


public:
    // 初始化时指定资源的初始数量
    constexpr Semaphore(int init_count = 0) : count_(init_count) {}
    
    void init(int init_count) {
        count_ = init_count;
        wait_mask_ = 0;
    }

    // 消费者等待资源
    void wait() {
        TaskControlBlock* current = Scheduler::instance().get_current_tcb();
        while (true) {
            Arch::disable_interrupts();
            if (count_ > 0) {
                count_--;
                wait_mask_ &= ~(1 << current->id);
                Arch::enable_interrupts();
                return; // 成功获取资源
            }
            wait_mask_ |= (1 << current->id);
            Scheduler::instance().set_task_state(current->id, TaskState::Suspended);
            // 必须在关中断状态下调用 schedule()，将 PendSV 挂起。
            // 当随后调用 Arch::enable_interrupts() 时，PendSV 才会立刻触发上下文切换，
            // 从而彻底消除 ISR 在间隙抢占导致信号丢失或错乱的竞态窗口期。
            Scheduler::instance().schedule();
            Arch::enable_interrupts();
        }
    }

    // 消费者尝试获取资源：非阻塞版本，资源不足时立即返回 false，不会让出 CPU 或休眠。
    // 可安全用于中断上下文（ISR）—— 与 signal() 一样只用关中断做临界区保护，
    // 不涉及任务调度/系统调用。
    bool try_wait() {
        Arch::disable_interrupts();
        if (count_ > 0) {
            count_--;
            Arch::enable_interrupts();
            return true;
        }
        Arch::enable_interrupts();
        return false;
    }

    // 生产者释放/增加资源
    void signal() {
        Arch::disable_interrupts();
        count_++;
        if (wait_mask_ != 0) {
            uint32_t best_id = 0xFFFFFFFF;
            uint8_t best_prio = 0;
            for (int i = 0; i < Scheduler::get_max_tasks(); i++) {
                if (wait_mask_ & (1U << i)) {
                    TaskControlBlock* t = Scheduler::instance().get_task_by_id(i);
                    if (t && t->state == TaskState::Suspended) {
                        uint8_t prio = static_cast<uint8_t>(t->current_priority);
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
            }
        }
        Arch::enable_interrupts();
        Scheduler::instance().schedule();
    }
};

#endif
