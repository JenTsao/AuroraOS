#ifndef SEMAPHORE_HPP
#define SEMAPHORE_HPP

#include "../task/task.hpp"

class Semaphore {
private:
    int count_;
    uint32_t wait_mask_ = 0;

public:
    // 初始化时指定资源的初始数量
    constexpr Semaphore(int init_count = 0) : count_(init_count), wait_mask_(0) {}

    void init(int init_count) {
        IrqGuard guard;
        count_ = init_count;
        wait_mask_ = 0;
    }

    int get_count() const {
        return count_;
    }

    // 消费者等待资源 (支持超时控制, 0xFFFFFFFF = 无限阻塞)
    bool wait(uint32_t timeout_ticks = 0xFFFFFFFF);

    // 消费者尝试获取资源：非阻塞版本，资源不足时立即返回 false
    bool try_wait();

    // 生产者释放/增加资源
    void signal();
};

#endif
