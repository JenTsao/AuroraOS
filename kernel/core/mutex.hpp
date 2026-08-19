#ifndef MUTEX_HPP
#define MUTEX_HPP

#include "../task/task.hpp"
#include "syscall.hpp"
#include "../interrupt/timer.hpp"

class Mutex {
private:
    volatile bool locked_ = false;
    TaskControlBlock* owner_ = nullptr;
    uint32_t recursive_count_ = 0;
    Mutex* next_held_ = nullptr;
    uint32_t wait_mask_ = 0;
    TaskPriority ceiling_prio_ = TaskPriority::Idle;

    uint8_t get_highest_waiter() {
        uint8_t max_prio = 0;
        for (int i = 0; i < Scheduler::get_max_tasks(); i++) {
            if (wait_mask_ & (1U << i)) {
                TaskControlBlock* t = Scheduler::instance().get_task_by_id(i);
                if (t && static_cast<uint8_t>(t->scheduler.current_priority) > max_prio) {
                    max_prio = static_cast<uint8_t>(t->scheduler.current_priority);
                }
            }
        }
        return max_prio;
    }

    // 跨任务死锁循环等待检测 (Wait-For Graph Cycle Detection)
    static bool check_deadlock(TaskControlBlock* current, Mutex* target_mutex) {
        if (!current || !target_mutex)
            return false;
        TaskControlBlock* check = target_mutex->owner_;
        int steps = 0;
        while (check && steps < Scheduler::get_max_tasks()) {
            if (check == current) {
                return true; // 发现循环等待闭环：死锁！
            }
            if (check->scheduler.waiting_on_mutex) {
                check = check->scheduler.waiting_on_mutex->owner_;
            } else {
                break;
            }
            steps++;
        }
        return false;
    }

    static void propagate_priority(TaskControlBlock* start_task) {
        TaskControlBlock* task = start_task;
        int steps = 0;
        while (task->scheduler.waiting_on_mutex && steps < Scheduler::get_max_tasks()) {
            Mutex* m = task->scheduler.waiting_on_mutex;
            TaskControlBlock* owner = m->owner_;
            if (!owner || owner == start_task)
                break;

            if (static_cast<uint8_t>(task->scheduler.current_priority) >
                static_cast<uint8_t>(owner->scheduler.current_priority)) {
                Scheduler::instance().set_task_priority(owner->scheduler.id, task->scheduler.current_priority);
                task = owner;
            } else {
                break;
            }
            steps++;
        }
    }

    static void recalculate_priority_chain(TaskControlBlock* start_task) {
        TaskControlBlock* task = start_task;
        int steps = 0;
        while (task && steps < Scheduler::get_max_tasks()) {
            uint8_t max_prio = static_cast<uint8_t>(task->scheduler.base_priority);
            Mutex* m = static_cast<Mutex*>(task->scheduler.held_mutexes);
            while (m) {
                // 1. 立即优先级天花板协议 (IPCP)
                if (static_cast<uint8_t>(m->ceiling_prio_) > max_prio) {
                    max_prio = static_cast<uint8_t>(m->ceiling_prio_);
                }
                // 2. 优先级继承协议 (PIP)
                uint8_t highest_waiter = m->get_highest_waiter();
                if (highest_waiter > max_prio) {
                    max_prio = highest_waiter;
                }
                m = m->next_held_;
            }

            if (max_prio != static_cast<uint8_t>(task->scheduler.current_priority)) {
                Scheduler::instance().set_task_priority(task->scheduler.id, static_cast<TaskPriority>(max_prio));
                if (task->scheduler.waiting_on_mutex && task->scheduler.waiting_on_mutex->owner_) {
                    task = task->scheduler.waiting_on_mutex->owner_;
                } else {
                    break;
                }
            } else {
                break;
            }
            steps++;
        }
    }

    // 唤醒最高优先级的等待者
    void wake_highest_waiter() {
        if (wait_mask_ == 0)
            return;
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
        }
    }

public:
    constexpr Mutex(TaskPriority ceiling = TaskPriority::Idle) : ceiling_prio_(ceiling) {}

    void set_ceiling(TaskPriority ceiling) {
        IrqGuard guard;
        ceiling_prio_ = ceiling;
    }

    TaskPriority get_ceiling() const {
        return ceiling_prio_;
    }

    Mutex* get_next_held() const {
        return next_held_;
    }

    bool lock(uint32_t timeout_ticks = 0xFFFFFFFF) {
        TaskControlBlock* current = Scheduler::instance().get_current_tcb();
        uint32_t start_tick = TimerManager::instance().get_current_tick();

        {
            IrqGuard guard;
            if (!locked_) {
                locked_ = true;
                owner_ = current;
                recursive_count_ = 1;
                if (owner_) {
                    this->next_held_ = static_cast<Mutex*>(owner_->scheduler.held_mutexes);
                    owner_->scheduler.held_mutexes = this;
                    // 立即优先级天花板协议 (IPCP) 提权
                    if (static_cast<uint8_t>(ceiling_prio_) > static_cast<uint8_t>(owner_->scheduler.current_priority)) {
                        Scheduler::instance().set_task_priority(owner_->scheduler.id, ceiling_prio_);
                    }
                }
                return true;
            } else if (owner_ && owner_ == current) {
                recursive_count_++;
                return true;
            }

            if (!current) {
                // 调度器未启动，但发生资源竞争，只能直接返回
                return false;
            }

            // 跨任务死锁闭环检测 (Deadlock Detection)
            if (check_deadlock(current, this)) {
                current->task.errno_val = 35; // EDEADLK
                return false; // 检测到死锁，安全中止加锁并返回
            }

            uint8_t wait_prio = static_cast<uint8_t>(current->scheduler.current_priority);

            current->scheduler.waiting_on_mutex = this;
            wait_mask_ |= (1 << current->scheduler.id);
            // 优先级继承传播
            if (owner_ && wait_prio > static_cast<uint8_t>(owner_->scheduler.current_priority)) {
                propagate_priority(current);
            }
        }

        while (true) {
            bool need_wait = false;
            {
                IrqGuard guard;
                if (!locked_) {
                    wait_mask_ &= ~(1 << current->scheduler.id);
                    current->scheduler.waiting_on_mutex = nullptr;
                    locked_ = true;
                    owner_ = current;
                    recursive_count_ = 1;
                    this->next_held_ = static_cast<Mutex*>(owner_->scheduler.held_mutexes);
                    owner_->scheduler.held_mutexes = this;
                    if (static_cast<uint8_t>(ceiling_prio_) > static_cast<uint8_t>(owner_->scheduler.current_priority)) {
                        Scheduler::instance().set_task_priority(owner_->scheduler.id, ceiling_prio_);
                    }
                    return true;
                } else if (owner_ && owner_ == current) {
                    wait_mask_ &= ~(1 << current->scheduler.id);
                    current->scheduler.waiting_on_mutex = nullptr;
                    recursive_count_++;
                    return true;
                }

                uint32_t elapsed = TimerManager::instance().get_current_tick() - start_tick;
                if (timeout_ticks != 0xFFFFFFFF && elapsed >= timeout_ticks) {
                    wait_mask_ &= ~(1 << current->scheduler.id);
                    current->scheduler.waiting_on_mutex = nullptr;
                    if (owner_)
                        recalculate_priority_chain(owner_);
                    return false;
                }

                wait_mask_ |= (1 << current->scheduler.id);
                if (timeout_ticks != 0xFFFFFFFF) {
                    current->scheduler.sleep_ticks = timeout_ticks - elapsed;
                    Scheduler::instance().set_task_state(current->scheduler.id, TaskState::Sleeping);
                } else {
                    Scheduler::instance().set_task_state(current->scheduler.id, TaskState::Suspended);
                }
                need_wait = true;
            }

            if (need_wait) {
                Scheduler::instance().schedule();
            }
        }
    }

    void unlock() {
        TaskControlBlock* current = Scheduler::instance().get_current_tcb();
        bool trigger_schedule = false;

        {
            IrqGuard guard;
            if (locked_ && owner_ == current) {
                recursive_count_--;
                if (recursive_count_ == 0) {
                    // 从持有的锁链表中移除自身
                    if (owner_) {
                        Mutex** curr_ptr = reinterpret_cast<Mutex**>(&owner_->scheduler.held_mutexes);
                        while (*curr_ptr) {
                            if (*curr_ptr == this) {
                                *curr_ptr = this->next_held_;
                                break;
                            }
                            curr_ptr = &(*curr_ptr)->next_held_;
                        }
                    }
                    this->next_held_ = nullptr;

                    TaskControlBlock* old_owner = owner_;
                    owner_ = nullptr;
                    locked_ = false;

                    // 重新计算原拥有者的优先级并恢复
                    if (old_owner) {
                        recalculate_priority_chain(old_owner);
                    }

                    // 唤醒最高优先级的等待者
                    wake_highest_waiter();

                    trigger_schedule = true;
                }
            }
        }

        if (trigger_schedule) {
            Scheduler::instance().schedule();
        }
    }

    void force_unlock(TaskControlBlock* target_owner) {
        IrqGuard guard;
        if (locked_ && owner_ == target_owner) {
            if (owner_) {
                Mutex** curr_ptr = reinterpret_cast<Mutex**>(&owner_->scheduler.held_mutexes);
                while (*curr_ptr) {
                    if (*curr_ptr == this) {
                        *curr_ptr = this->next_held_;
                        break;
                    }
                    curr_ptr = &(*curr_ptr)->next_held_;
                }
            }
            this->next_held_ = nullptr;

            owner_ = nullptr;
            locked_ = false;
            recursive_count_ = 0;

            if (target_owner) {
                recalculate_priority_chain(target_owner);
            }
            wake_highest_waiter();
        }
    }
};

// CP.20: Use RAII, never plain lock()/unlock()
struct LockGuard {
    Mutex& m_;

    explicit LockGuard(Mutex& m) : m_(m) {
        m_.lock();
    }

    ~LockGuard() {
        m_.unlock();
    }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
};

struct UniqueLock {
    Mutex& m_;
    bool owns_lock_;

    explicit UniqueLock(Mutex& m, uint32_t timeout_ticks = 0xFFFFFFFF) : m_(m) {
        owns_lock_ = m_.lock(timeout_ticks);
    }

    ~UniqueLock() {
        if (owns_lock_) {
            m_.unlock();
        }
    }

    bool owns_lock() const {
        return owns_lock_;
    }

    UniqueLock(const UniqueLock&) = delete;
    UniqueLock& operator=(const UniqueLock&) = delete;
};

#endif
