#ifndef AURORA_TIMER_HPP
#define AURORA_TIMER_HPP

#include <stdint.h>
#include "../task/task.hpp"
#include "../core/semaphore.hpp"

// 定时器工作模式
enum class TimerType {
    OneShot,  // 单次触发
    Periodic  // 周期性触发
};

// 定时器回调函数签名，允许携带一个自定义的透传参数
using TimerCallback = void (*)(void* arg);

struct SoftwareTimer {
    bool active;
    TimerType type;
    uint32_t expire_tick;     // 绝对到期时间 (系统的全局 tick 值)
    uint32_t period_ticks;    // 周期时间
    TimerCallback callback;
    void* arg;
};

class TimerManager {
private:
    static constexpr int MAX_TIMERS = 8;
    SoftwareTimer timers_[MAX_TIMERS];
    
    // 用于唤醒守护线程的二值信号量
    Semaphore wakeup_sem_{0};
    uint32_t current_tick_ = 0;

    TimerManager() {
        for (int i = 0; i < MAX_TIMERS; i++) timers_[i].active = false;
    }

public:
    static TimerManager& instance() {
        static TimerManager mgr;
        return mgr;
    }

    uint32_t get_current_tick() const { return current_tick_; }

    // 获取下一个最早到期定时器的剩余时间 (Tickless Idle 预测)
    uint32_t get_next_expire_ticks() const {
        uint32_t min_ticks = 0xFFFFFFFF;
        for (int i = 0; i < MAX_TIMERS; i++) {
            if (timers_[i].active) {
                uint32_t remaining = (timers_[i].expire_tick > current_tick_) ? 
                                     (timers_[i].expire_tick - current_tick_) : 0;
                if (remaining < min_ticks) min_ticks = remaining;
            }
        }
        return min_ticks;
    }

    // Tickless 休眠唤醒后的时间补偿
    void fast_forward_ticks(uint32_t ticks) {
        current_tick_ += ticks;
        // 如果有定时器因此到期，这里也可以选择立刻触发 wakeup_sem_
        bool need_wakeup = false;
        for (int i = 0; i < MAX_TIMERS; i++) {
            if (timers_[i].active && current_tick_ >= timers_[i].expire_tick) {
                need_wakeup = true;
                break;
            }
        }
        if (need_wakeup) {
            wakeup_sem_.signal(); 
        }
    }

    // 1. 供应用层调用的 API：创建并启动定时器
    int start_timer(uint32_t period_ticks, TimerType type, TimerCallback cb, void* arg = nullptr) {
        IrqGuard guard;
        for (int i = 0; i < MAX_TIMERS; i++) {
            if (!timers_[i].active) {
                timers_[i].period_ticks = period_ticks;
                timers_[i].type = type;
                timers_[i].callback = cb;
                timers_[i].arg = arg;
                timers_[i].expire_tick = current_tick_ + period_ticks;
                timers_[i].active = true;
                
                return i; // 返回定时器 ID
            }
        }
        return -1; // 定时器槽位已满
    }

    void stop_timer(int id) {
        if (id >= 0 && id < MAX_TIMERS) {
            IrqGuard guard;
            timers_[id].active = false;
        }
    }

    // 2. 供 SysTick 中断调用的极简硬件钩子
    void on_tick() {
        current_tick_++;
        bool need_wakeup = false;
        
        for (int i = 0; i < MAX_TIMERS; i++) {
            // 检查是否有处于激活状态且已经到期的定时器
            if (timers_[i].active && current_tick_ >= timers_[i].expire_tick) {
                need_wakeup = true;
                break;
            }
        }
        
        // 如果有定时器到期，立刻发送信号量唤醒后台的 C++ 守护线程
        if (need_wakeup) {
            wakeup_sem_.signal(); 
        }
    }

    // 3. 定时器守护线程 (Timer Daemon) 的执行中枢
    void daemon_task() {
        while (true) {
            // 绝大多数时间，这个线程都在这里 0 功耗休眠阻塞
            wakeup_sem_.wait(); 

            // 【修复 BUG #6】循环检查定时器，直到所有到期定时器都被处理。
            // 原实现只扫描一次，若回调执行期间有新的定时器到期则跳过。
            bool has_expired = true;
            while (has_expired) {
                has_expired = false;
                uint32_t tick_now;
                {
                    IrqGuard guard;
                    tick_now = current_tick_;
                }

                for (int i = 0; i < MAX_TIMERS; i++) {
                    TimerCallback cb = nullptr;
                    void* arg = nullptr;
                    
                    {
                        IrqGuard guard;
                        if (timers_[i].active && tick_now >= timers_[i].expire_tick) {
                            cb = timers_[i].callback;
                            arg = timers_[i].arg;
                            
                            // b. 根据模式决定是自动重启还是销毁
                            if (timers_[i].type == TimerType::Periodic) {
                                timers_[i].expire_tick += timers_[i].period_ticks; // 避免时钟漂移
                                // 如果重置后 tick 已经过了新的到期时间，下一轮需再次处理
                                if (tick_now >= timers_[i].expire_tick) {
                                    has_expired = true;
                                }
                            } else {
                                timers_[i].active = false;
                            }
                        }
                    }
                    
                    // a. 真正执行用户的耗时回调（脱离了中断上下文，极其安全！）
                    if (cb) {
                        cb(arg);
                        has_expired = true;  // 回调可能耗时，重新检查是否还有到期定时器
                    }
                }
            }
        }
    }
};

#endif
