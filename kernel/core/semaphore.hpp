#ifndef SEMAPHORE_HPP
#define SEMAPHORE_HPP

#include "../task/task.hpp"
#include "../task/wait_queue.hpp"

class Semaphore {
private:
    int count_;
    auroraos::kernel::WaitQueue wait_queue_;

public:
    // 初始化时指定资源的初始数量与等待队列策略（默认按动态优先级 + 同级 FIFO，亦可配置为纯 FIFO）
    constexpr Semaphore(int init_count = 0,
                        auroraos::kernel::WaitPolicy policy = auroraos::kernel::WaitPolicy::Priority)
        : count_(init_count), wait_queue_(policy) {}

    void init(int init_count, auroraos::kernel::WaitPolicy policy = auroraos::kernel::WaitPolicy::Priority) {
        IrqGuard guard;
        count_ = init_count;
        wait_queue_.clear();
        wait_queue_.set_policy(policy);
    }

    void set_wait_policy(auroraos::kernel::WaitPolicy policy) {
        IrqGuard guard;
        wait_queue_.set_policy(policy);
    }

    auroraos::kernel::WaitPolicy get_wait_policy() const {
        return wait_queue_.policy();
    }

    auroraos::kernel::WaitQueue& wait_queue() noexcept {
        return wait_queue_;
    }

    const auroraos::kernel::WaitQueue& wait_queue() const noexcept {
        return wait_queue_;
    }

    int get_count() const {
        return count_;
    }

    // 消费者等待资源 (支持超时控制, 0xFFFFFFFF = 无限阻塞)
    bool wait(uint32_t timeout_ticks = 0xFFFFFFFF);

    // 消费者尝试获取资源：非阻塞版本，资源不足时立即返回 false
    bool try_wait();

    // 生产者释放/增加资源。
    // in_isr=true 时仅做唤醒（置 Ready），不触发 schedule()——中断上下文
    // 禁止直接调度（SysTick 尾部会统一调度；外设 ISR 靠 PendSV 悬挂）。
    void signal(bool in_isr = false);
};

#endif
