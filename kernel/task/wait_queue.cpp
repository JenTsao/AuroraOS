#include "wait_queue.hpp"
#include "task.hpp"

namespace auroraos {
namespace kernel {

void WaitQueue::enqueue(TaskControlBlock* task) {
    if (!task)
        return;
    task->ipc.blocked_next = nullptr;

    // 若队列为空，或新任务优先级严格高于当前队头任务，直接作为新队头
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
    while (curr->ipc.blocked_next &&
           static_cast<uint8_t>(curr->ipc.blocked_next->scheduler.current_priority) >=
               static_cast<uint8_t>(task->scheduler.current_priority)) {
        curr = curr->ipc.blocked_next;
    }

    task->ipc.blocked_next = curr->ipc.blocked_next;
    curr->ipc.blocked_next = task;
    if (curr == tail_) {
        tail_ = task;
    }
}

TaskControlBlock* WaitQueue::dequeue() {
    if (!head_)
        return nullptr;
    TaskControlBlock* task = head_;
    head_ = task->ipc.blocked_next;
    if (!head_)
        tail_ = nullptr;
    task->ipc.blocked_next = nullptr;
    return task;
}

bool WaitQueue::remove(TaskControlBlock* task) {
    if (!head_ || !task)
        return false;

    if (head_ == task) {
        head_ = task->ipc.blocked_next;
        if (!head_)
            tail_ = nullptr;
        task->ipc.blocked_next = nullptr;
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
        return true;
    }

    return false;
}

TaskControlBlock* WaitQueue::dequeue_matching_sender(uint32_t label_filter) {
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
            return curr;
        }
        prev = curr;
        curr = curr->ipc.blocked_next;
    }

    return nullptr;
}

TaskControlBlock* WaitQueue::dequeue_matching_receiver(uint32_t sender_label) {
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
            return curr;
        }
        prev = curr;
        curr = curr->ipc.blocked_next;
    }

    return nullptr;
}

} // namespace kernel
} // namespace auroraos
