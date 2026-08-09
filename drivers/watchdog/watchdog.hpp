#ifndef AURORA_WATCHDOG_HPP
#define AURORA_WATCHDOG_HPP

#include <stdint.h>

enum class WatchdogMode : uint8_t {
    Interrupt,  // 超时前产生中断，允许喂狗恢复
    Reset,      // 超时直接硬件复位（生产模式）
    Both        // 先中断，中断未处理则复位
};

class WatchdogDriver {
public:
    virtual ~WatchdogDriver() = default;
    virtual bool init(uint32_t timeout_ms, WatchdogMode mode = WatchdogMode::Reset) = 0;
    virtual void kick() = 0;
    virtual void disable() = 0;
    virtual uint32_t get_remaining() const = 0;

    // Query whether the watchdog interrupt has fired since last init.
    virtual bool had_interrupt() const { return false; }

    // Called from SysTick_Handler every tick. Hardware WDTs are independent
    // and need no tick; software WDTs decrement here and return true on expiry.
    virtual bool on_tick() { return false; }
};

#endif
