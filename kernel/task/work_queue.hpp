#ifndef AURORA_WORK_QUEUE_HPP
#define AURORA_WORK_QUEUE_HPP

#include <stdint.h>
#include "task.hpp"
#include "semaphore.hpp"

// 定义工作项的回调函数签名，允许携带透传参数
using WorkCallback = void (*)(void* arg);

// 工作项结构体
struct WorkItem {
    WorkCallback callback;
    void* arg;
};

class WorkQueue {
private:
    static constexpr int CAPACITY = 16;
    WorkItem buffer_[CAPACITY];
    int head_ = 0;
    int tail_ = 0;
    int count_ = 0;

    // 信号量：用于通知守护线程有新工作到达
    Semaphore items_{0}; 

public:
    static WorkQueue& instance() {
        static WorkQueue wq;
        return wq;
    }

    // ========================================================
    // 供【硬件中断 ISR】调用的极速提交接口 (非阻塞！)
    // ========================================================
    bool submit_from_isr(WorkCallback cb, void* arg = nullptr) {
        bool success = false;
        
        Arch::disable_interrupts(); // 短暂保护临界区
        if (count_ < CAPACITY) {
            buffer_[tail_].callback = cb;
            buffer_[tail_].arg = arg;
            tail_ = (tail_ + 1) % CAPACITY;
            count_++;
            success = true;
        }
        Arch::enable_interrupts();

        // 如果提交成功，发射信号量唤醒后台守护线程
        if (success) {
            items_.signal(); 
        }
        return success;
    }

    // ========================================================
    // 供【工作队列守护线程】执行的中枢 (运行在普通任务上下文)
    // ========================================================
    void worker_task() {
        while (true) {
            // 如果没有工作，线程会在这里 0 功耗挂起休眠
            items_.wait(); 

            WorkItem item;
            Arch::disable_interrupts();
            item = buffer_[head_];
            head_ = (head_ + 1) % CAPACITY;
            count_--;
            Arch::enable_interrupts();

            // 脱离临界区，在这个普通线程的安全上下文中执行耗时任务！
            // 因为在线程上下文，所以回调内部可以使用 sleep()、lock() 甚至文件读写
            if (item.callback) {
                item.callback(item.arg);
            }
        }
    }
    
private:
    WorkQueue() = default;
};

#endif
