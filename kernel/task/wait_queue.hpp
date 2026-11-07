#ifndef WAIT_QUEUE_HPP
#define WAIT_QUEUE_HPP

#include <stdint.h>

struct TaskControlBlock;

namespace auroraos {
namespace kernel {

// 统一等待队列入队策略：
//   - Fifo:     按入队先后顺序唤醒（严格 FIFO 唤醒序）
//   - Priority: 按任务动态优先级 (current_priority) 降序排列，
//               同优先级任务严格维持 FIFO 先来后到
// 语义边界：本队列只保证「唤醒/出队顺序」，不保证「资源获取顺序」——
// 各原语的 lock/post 快路径允许新到者在被唤醒者重新调度前抢先获取
// （barging），Fifo 策略下同级任务仍可能存在获取阶段的插队。
enum class WaitPolicy : uint8_t {
    Fifo = 0,
    Priority = 1
};

class WaitQueue {
public:
    constexpr explicit WaitQueue(WaitPolicy policy = WaitPolicy::Priority) noexcept
        : head_(nullptr), tail_(nullptr), policy_(policy) {}

    // 析构清队：解除所有残留等待者的 waiting_queue 绑定，防止队列对象先于
    // 等待者销毁时，后续 enqueue 解引用悬垂指针（use-after-free）。
    ~WaitQueue() noexcept {
        clear();
    }

    constexpr WaitPolicy policy() const noexcept {
        return policy_;
    }

    // 切换队列策略；若切换至 Priority 且队列非空，自动按当前动态优先级重排
    void set_policy(WaitPolicy policy) noexcept;

    // 将任务按当前策略（FIFO 或 Priority）挂入等待队列，并绑定 task->scheduler.waiting_queue
    void enqueue(TaskControlBlock* task) noexcept;

    // 弹出队首任务并解除 waiting_queue 绑定
    TaskControlBlock* dequeue() noexcept;

    // 从队列中摘除指定任务并解除 waiting_queue 绑定
    bool remove(TaskControlBlock* task) noexcept;

    // 清空队列中所有等待节点并解除绑定
    void clear() noexcept;

    // 当队列中某阻塞任务的动态优先级发生变化（如传递性 PIP 提权/恢复）时，
    // 由 Scheduler::set_task_priority 自动回调以维持 Priority 队列严格有序
    void on_priority_changed(TaskControlBlock* task) noexcept;

    void reorder(TaskControlBlock* task) noexcept {
        on_priority_changed(task);
    }

    // 获取队列中最高等待者的动态优先级（空队列返回 0；Priority 策略下为 O(1)）
    uint8_t highest_priority() const noexcept;

    // 判断任务当前是否在本等待队列中
    bool contains(const TaskControlBlock* task) const noexcept;

    // 当前等待队列中的任务数量
    uint32_t size() const noexcept;

    // 弹出并唤醒队首有效等待任务（置为 TaskState::Ready），返回被唤醒任务指针
    TaskControlBlock* wake_one() noexcept;

    // 唤醒队列中的所有等待任务，返回唤醒的任务数量
    uint32_t wake_all() noexcept;

    // 根据 Label 过滤器选择性匹配出队（供 IPC Endpoint 使用）
    TaskControlBlock* dequeue_matching_sender(uint32_t label_filter) noexcept;
    TaskControlBlock* dequeue_matching_receiver(uint32_t sender_label) noexcept;

    constexpr bool empty() const noexcept {
        return head_ == nullptr;
    }

    constexpr TaskControlBlock* head() const noexcept {
        return head_;
    }

    constexpr TaskControlBlock* tail() const noexcept {
        return tail_;
    }

private:
    TaskControlBlock* head_{nullptr};
    TaskControlBlock* tail_{nullptr};
    WaitPolicy policy_{WaitPolicy::Priority};
};

} // namespace kernel
} // namespace auroraos

#endif // WAIT_QUEUE_HPP
