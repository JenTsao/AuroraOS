#ifndef MSG_QUEUE_HPP
#define MSG_QUEUE_HPP

#include "semaphore.hpp"

// 裸机环境下的定长泛型无锁单生产者单消费者 (SPSC) 消息队列
template <typename T, int Capacity> class MessageQueue {
private:
    T buffer_[Capacity];
    volatile int head_ = 0;
    volatile int tail_ = 0;

    Semaphore items_;
    Semaphore spaces_;

public:
    constexpr MessageQueue() : buffer_{}, items_(0), spaces_(Capacity) {}

    void init() {
        IrqGuard guard;
        head_ = 0;
        tail_ = 0;
        items_.init(0);
        spaces_.init(Capacity);
    }

    // 生产者调用：向队列发送消息（追加到队尾）
    void push(const T& item) {
        spaces_.wait(); // 如果队列满了，生产者会在这里阻塞休眠

        {
            IrqGuard guard;
            int current_tail = tail_;
            buffer_[current_tail] = item;
            tail_ = (current_tail + 1) % Capacity;
        }

        items_.signal(); // 通知消费者：有新消息啦！
    }

    // 消费者调用：从队列获取消息
    T pop() {
        items_.wait(); // 如果队列空了，消费者会在这里阻塞休眠等待

        T item;
        {
            IrqGuard guard;
            int current_head = head_;
            item = buffer_[current_head];
            head_ = (current_head + 1) % Capacity;
        }

        spaces_.signal(); // 通知生产者：腾出一个空槽位啦！
        return item;
    }

    // 带超时的消费者调用：超时返回 false，否则写入 out 并返回 true
    bool pop_timeout(T& out, uint32_t timeout_ticks) {
        if (!items_.wait(timeout_ticks)) {
            return false;
        }

        {
            IrqGuard guard;
            int current_head = head_;
            out = buffer_[current_head];
            head_ = (current_head + 1) % Capacity;
        }

        spaces_.signal();
        return true;
    }

    // 非阻塞生产者：队列已满时立即返回 false
    bool try_push(const T& item) {
        if (!spaces_.try_wait())
            return false; // 队列已满，不阻塞

        {
            IrqGuard guard;
            int current_tail = tail_;
            buffer_[current_tail] = item;
            tail_ = (current_tail + 1) % Capacity;
        }

        items_.signal();
        return true;
    }

    // 非阻塞消费者
    bool try_pop(T& out) {
        if (!items_.try_wait())
            return false; // 队列为空，不阻塞

        {
            IrqGuard guard;
            int current_head = head_;
            out = buffer_[current_head];
            head_ = (current_head + 1) % Capacity;
        }

        spaces_.signal();
        return true;
    }
};

#endif
