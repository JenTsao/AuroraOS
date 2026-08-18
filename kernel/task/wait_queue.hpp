#ifndef WAIT_QUEUE_HPP
#define WAIT_QUEUE_HPP

#include <stdint.h>

struct TaskControlBlock;

namespace auroraos {
namespace kernel {

class WaitQueue {
public:
    WaitQueue() = default;

    void enqueue(TaskControlBlock* task);
    TaskControlBlock* dequeue();
    bool remove(TaskControlBlock* task);

    // 根据 Label 过滤器选择性匹配出队
    TaskControlBlock* dequeue_matching_sender(uint32_t label_filter);
    TaskControlBlock* dequeue_matching_receiver(uint32_t sender_label);

    bool empty() const {
        return head_ == nullptr;
    }

    TaskControlBlock* head() const {
        return head_;
    }

private:
    TaskControlBlock* head_{nullptr};
    TaskControlBlock* tail_{nullptr};
};

} // namespace kernel
} // namespace auroraos

#endif // WAIT_QUEUE_HPP
