#include "wait_queue.hpp"
#include "task.hpp"

namespace auroraos {
namespace kernel {

void WaitQueue::enqueue(TaskControlBlock* task) {
    if (!task) return;
    task->ipc.blocked_next = nullptr;
    if (!tail_) {
        head_ = tail_ = task;
    } else {
        tail_->ipc.blocked_next = task;
        tail_ = task;
    }
}

TaskControlBlock* WaitQueue::dequeue() {
    if (!head_) return nullptr;
    TaskControlBlock* task = head_;
    head_ = task->ipc.blocked_next;
    if (!head_) tail_ = nullptr;
    task->ipc.blocked_next = nullptr;
    return task;
}

} // namespace kernel
} // namespace auroraos
