#include "wait_queue.hpp"
#include "task.hpp"

namespace auroraos {
namespace kernel {

void WaitQueue::set_policy(WaitPolicy policy) noexcept {
    if (policy_ == policy)
        return;
    policy_ = policy;
    if (policy_ == WaitPolicy::Priority && head_ && head_ != tail_) {
        TaskControlBlock* list = head_;
        head_ = nullptr;
        tail_ = nullptr;
        while (list) {
            TaskControlBlock* next = list->ipc.blocked_next;
            list->ipc.blocked_next = nullptr;
            list->scheduler.waiting_queue = nullptr;
            enqueue(list);
            list = next;
        }
    }
}

void WaitQueue::enqueue(TaskControlBlock* task) noexcept {
    if (!task)
        return;

    // 防御重入队自环：若任务已处于某个等待队列，先将其安全摘除
    if (task->scheduler.waiting_queue != nullptr) {
        task->scheduler.waiting_queue->remove(task);
    } else if (contains(task)) {
        remove(task);
    }

    task->ipc.blocked_next = nullptr;
    task->scheduler.waiting_queue = this;

    if (policy_ == WaitPolicy::Fifo) {
        if (!head_) {
            head_ = task;
            tail_ = task;
        } else {
            tail_->ipc.blocked_next = task;
            tail_ = task;
        }
        return;
    }

    // WaitPolicy::Priority：若队列为空，或新任务优先级严格高于当前队头任务，直接作为新队头
    if (!head_ || static_cast<uint8_t>(task->scheduler.current_priority) >
                      static_cast<uint8_t>(head_->scheduler.current_priority)) {
        task->ipc.blocked_next = head_;
        head_ = task;
        if (!tail_) {
            tail_ = task;
        }
        return;
    }

    // 否则按优先级降序寻找插入位置 (相同优先级下使用 >= 维持先来后到的 FIFO 稳定性)
    TaskControlBlock* curr = head_;
    while (curr->ipc.blocked_next && static_cast<uint8_t>(curr->ipc.blocked_next->scheduler.current_priority) >=
                                         static_cast<uint8_t>(task->scheduler.current_priority)) {
        curr = curr->ipc.blocked_next;
    }

    task->ipc.blocked_next = curr->ipc.blocked_next;
    curr->ipc.blocked_next = task;
    if (curr == tail_) {
        tail_ = task;
    }
}

TaskControlBlock* WaitQueue::dequeue() noexcept {
    if (!head_)
        return nullptr;
    TaskControlBlock* task = head_;
    head_ = task->ipc.blocked_next;
    if (!head_)
        tail_ = nullptr;
    task->ipc.blocked_next = nullptr;
    if (task->scheduler.waiting_queue == this) {
        task->scheduler.waiting_queue = nullptr;
    }
    return task;
}

bool WaitQueue::remove(TaskControlBlock* task) noexcept {
    if (!head_ || !task)
        return false;

    if (head_ == task) {
        head_ = task->ipc.blocked_next;
        if (!head_)
            tail_ = nullptr;
        task->ipc.blocked_next = nullptr;
        if (task->scheduler.waiting_queue == this) {
            task->scheduler.waiting_queue = nullptr;
        }
        return true;
    }

    TaskControlBlock* curr = head_;
    while (curr->ipc.blocked_next && curr->ipc.blocked_next != task) {
        curr = curr->ipc.blocked_next;
    }

    if (curr->ipc.blocked_next == task) {
        curr->ipc.blocked_next = task->ipc.blocked_next;
        if (tail_ == task) {
            tail_ = curr;
        }
        task->ipc.blocked_next = nullptr;
        if (task->scheduler.waiting_queue == this) {
            task->scheduler.waiting_queue = nullptr;
        }
        return true;
    }

    return false;
}

void WaitQueue::clear() noexcept {
    TaskControlBlock* curr = head_;
    head_ = nullptr;
    tail_ = nullptr;
    while (curr) {
        TaskControlBlock* next = curr->ipc.blocked_next;
        curr->ipc.blocked_next = nullptr;
        if (curr->scheduler.waiting_queue == this) {
            curr->scheduler.waiting_queue = nullptr;
        }
        curr = next;
    }
}

void WaitQueue::on_priority_changed(TaskControlBlock* task) noexcept {
    if (policy_ != WaitPolicy::Priority || !task || !head_ || head_ == tail_)
        return;

    if (remove(task)) {
        enqueue(task);
    }
}

uint8_t WaitQueue::highest_priority() const noexcept {
    if (!head_)
        return 0;
    if (policy_ == WaitPolicy::Priority) {
        return static_cast<uint8_t>(head_->scheduler.current_priority);
    }
    uint8_t max_prio = 0;
    for (const TaskControlBlock* curr = head_; curr != nullptr; curr = curr->ipc.blocked_next) {
        uint8_t prio = static_cast<uint8_t>(curr->scheduler.current_priority);
        if (prio > max_prio) {
            max_prio = prio;
        }
    }
    return max_prio;
}

bool WaitQueue::contains(const TaskControlBlock* task) const noexcept {
    if (!task)
        return false;
    for (const TaskControlBlock* curr = head_; curr != nullptr; curr = curr->ipc.blocked_next) {
        if (curr == task)
            return true;
    }
    return false;
}

uint32_t WaitQueue::size() const noexcept {
    uint32_t count = 0;
    for (const TaskControlBlock* curr = head_; curr != nullptr; curr = curr->ipc.blocked_next) {
        ++count;
    }
    return count;
}

TaskControlBlock* WaitQueue::wake_one() noexcept {
    while (!empty()) {
        TaskControlBlock* task = dequeue();
        if (task && task->scheduler.state != TaskState::Unallocated && task->scheduler.state != TaskState::Terminated) {
            task->scheduler.sleep_ticks = 0;
            Scheduler::instance().set_task_state(task->scheduler.id, TaskState::Ready);
            return task;
        }
    }
    return nullptr;
}

uint32_t WaitQueue::wake_all() noexcept {
    uint32_t woken = 0;
    while (wake_one() != nullptr) {
        ++woken;
    }
    return woken;
}

TaskControlBlock* WaitQueue::dequeue_matching_sender(uint32_t label_filter) noexcept {
    if (!head_)
        return nullptr;

    if (label_filter == 0 || head_->ipc.msg_type == label_filter) {
        return dequeue();
    }

    TaskControlBlock* prev = head_;
    TaskControlBlock* curr = head_->ipc.blocked_next;

    while (curr) {
        if (curr->ipc.msg_type == label_filter) {
            prev->ipc.blocked_next = curr->ipc.blocked_next;
            if (tail_ == curr) {
                tail_ = prev;
            }
            curr->ipc.blocked_next = nullptr;
            if (curr->scheduler.waiting_queue == this) {
                curr->scheduler.waiting_queue = nullptr;
            }
            return curr;
        }
        prev = curr;
        curr = curr->ipc.blocked_next;
    }

    return nullptr;
}

TaskControlBlock* WaitQueue::dequeue_matching_receiver(uint32_t sender_label) noexcept {
    if (!head_)
        return nullptr;

    if (head_->ipc.label_filter == 0 || head_->ipc.label_filter == sender_label) {
        return dequeue();
    }

    TaskControlBlock* prev = head_;
    TaskControlBlock* curr = head_->ipc.blocked_next;

    while (curr) {
        if (curr->ipc.label_filter == 0 || curr->ipc.label_filter == sender_label) {
            prev->ipc.blocked_next = curr->ipc.blocked_next;
            if (tail_ == curr) {
                tail_ = prev;
            }
            curr->ipc.blocked_next = nullptr;
            if (curr->scheduler.waiting_queue == this) {
                curr->scheduler.waiting_queue = nullptr;
            }
            return curr;
        }
        prev = curr;
        curr = curr->ipc.blocked_next;
    }

    return nullptr;
}

} // namespace kernel
} // namespace auroraos
