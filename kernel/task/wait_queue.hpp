#ifndef WAIT_QUEUE_HPP
#define WAIT_QUEUE_HPP

struct TaskControlBlock;

namespace auroraos {
namespace kernel {

class WaitQueue {
public:
    WaitQueue() = default;

    void enqueue(TaskControlBlock* task);
    TaskControlBlock* dequeue();

    bool empty() const {
        return head_ == nullptr;
    }

private:
    TaskControlBlock* head_{nullptr};
    TaskControlBlock* tail_{nullptr};
};

} // namespace kernel
} // namespace auroraos

#endif // WAIT_QUEUE_HPP
