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
    // constexpr: 保证多态全局对象 (如 apps/kernel.cpp 的 g_soft_wdt) 走
    // 常量初始化路径 —— vptr 在链接镜像 (.data) 中静态写入。本固件启动
    // 不运行任何静态构造函数 (无 .init_array 调用)，若 ctor 不可折叠，
    // vptr 将落在 .bss 中为 0，SysTick 首次虚调用即跳转到 0 (INVSTATE
    // HardFault，Cortex-M7 -Oz 构建实测触发；-O2 构建会被折叠而幸免)。
    constexpr SoftWdt() : remaining_ticks_(0), reload_ticks_(0), enabled_(false) {}

    bool init(uint32_t timeout_ms, WatchdogMode mode = WatchdogMode::Reset) override {
        (void)mode;
        reload_ticks_ = timeout_ms; // 1 tick = 1ms at 1000Hz
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
    bool on_tick() override {
        if (!enabled_)
            return false;
        if (remaining_ticks_ > 0) {
            remaining_ticks_--;
        }
        return remaining_ticks_ == 0;
    }
};

#endif
