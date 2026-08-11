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
    }

    WatchdogDriver* get_driver() { return driver_; }

    void on_schedule(uint32_t next_task_priority) {
        if (!driver_) return;
        if (next_task_priority == 0) {
            current_idle_++;
        } else {
            current_idle_ = 0;
        }
        driver_->kick();
    }

    void kick() {
        current_idle_ = 0;
        if (driver_) driver_->kick();
    }

    void disable() {
        if (driver_) driver_->disable();
    }
};

// Implementation of the weak symbol declared in task.hpp.
// When watchdog_manager.hpp is included in the build, this overrides the
// default no-op and routes to the singleton.
inline void watchdog_feed(uint32_t task_priority) {
    WatchdogManager::instance().on_schedule(task_priority);
}

#endif
