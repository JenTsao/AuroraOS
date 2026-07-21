#ifndef SOFT_WDT_HPP
#define SOFT_WDT_HPP

#include "watchdog.hpp"

// Software watchdog for platforms without hardware WDT (e.g. QEMU RISC-V).
// Uses a tick counter decremented by SysTick; triggers kernel panic on expiry.
class SoftWdt : public WatchdogDriver {
private:
    uint32_t remaining_ticks_ = 0;
    uint32_t reload_ticks_ = 0;
    bool enabled_ = false;

public:
    bool init(uint32_t timeout_ms, WatchdogMode mode = WatchdogMode::Reset) override {
        (void)mode;
        reload_ticks_ = timeout_ms;  // 1 tick = 1ms at 1000Hz
        remaining_ticks_ = reload_ticks_;
        enabled_ = true;
        return true;
    }

    void kick() override {
        remaining_ticks_ = reload_ticks_;
    }

    void disable() override {
        enabled_ = false;
    }

    uint32_t get_remaining() const override {
        return remaining_ticks_;
    }

    // Called from SysTick_Handler every tick. Returns true if expired.
    bool on_tick() {
        if (!enabled_) return false;
        if (remaining_ticks_ > 0) {
            remaining_ticks_--;
        }
        return remaining_ticks_ == 0;
    }
};

#endif
