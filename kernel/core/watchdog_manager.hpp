#ifndef AURORA_WATCHDOG_MANAGER_HPP
#define AURORA_WATCHDOG_MANAGER_HPP

#include "drivers/watchdog/watchdog.hpp"
#include "task.hpp"

class WatchdogManager {
private:
    WatchdogDriver* driver_ = nullptr;
    uint32_t max_idle_ticks_ = 0;
    uint32_t current_idle_ = 0;

public:
    static WatchdogManager& instance() {
        static WatchdogManager mgr;
        return mgr;
    }

    void init(WatchdogDriver* driver, uint32_t timeout_ms) {
        driver_ = driver;
        max_idle_ticks_ = (timeout_ms * 8) / 10; // 80% threshold
        current_idle_ = 0;
        if (driver_) {
            driver_->init(timeout_ms);
        }
    }

    WatchdogDriver* get_driver() {
        return driver_;
    }

    void on_schedule(uint32_t next_task_priority) {
        if (!driver_)
            return;
        if (next_task_priority == 0) {
            current_idle_++;
        } else {
            current_idle_ = 0;
        }
        driver_->kick();
    }

    void kick() {
        current_idle_ = 0;
        if (driver_)
            driver_->kick();
    }

    void disable() {
        if (driver_)
            driver_->disable();
    }
};

// 注意：watchdog_feed 的定义**不在这里**。
// 头文件里的 inline 版本是弱符号（COMDAT），无法可靠地为
// Scheduler::schedule() 发出的强引用提供定义：它只在某个 TU 恰好 odr-use
// 它时才被发射，且与 watchdog_stub.cpp 的弱实现并存时链接结果依赖目标文件
// 顺序。唯一、确定的定义在 kernel/core/watchdog_stub.cpp。

#endif
