#ifndef LM3S_WDT_HPP
#define LM3S_WDT_HPP

#include "watchdog.hpp"
#include "board.h"

// TI LM3S6965 Watchdog Timer 寄存器 (StellarisWare)
class Lm3sWdt : public WatchdogDriver {
private:
    static constexpr uintptr_t WDT_LOAD = BOARD_WDT_BASE + 0x000;
    static constexpr uintptr_t WDT_VALUE = BOARD_WDT_BASE + 0x004;
    static constexpr uintptr_t WDT_CTL = BOARD_WDT_BASE + 0x008;
    static constexpr uintptr_t WDT_ICR = BOARD_WDT_BASE + 0x00C;
    static constexpr uintptr_t WDT_RIS = BOARD_WDT_BASE + 0x010;
    static constexpr uintptr_t WDT_MIS = BOARD_WDT_BASE + 0x014;
    static constexpr uintptr_t WDT_LOCK = BOARD_WDT_BASE + 0xC00;

    static constexpr uint32_t WDT_CTL_INTEN = (1UL << 0);
    static constexpr uint32_t WDT_CTL_RESEN = (1UL << 1);
    static constexpr uint32_t WDT_LOCK_UNLOCK = 0x1ACCE551U;

    uint32_t load_val_ = 0;
    bool interrupt_fired_ = false;

    static volatile uint32_t& reg(uint32_t offset) {
        return *reinterpret_cast<volatile uint32_t*>(offset);
    }

    void unlock_regs() {
        reg(WDT_LOCK) = WDT_LOCK_UNLOCK;
    }

public:
    bool init(uint32_t timeout_ms, WatchdogMode mode = WatchdogMode::Reset) override {
        interrupt_fired_ = false;
        unlock_regs();

        // 64位运算防止 timeout_ms * (SYSCLK / 1000) 溢出 UINT32_MAX
        const uint64_t ticks = static_cast<uint64_t>(timeout_ms) * (BOARD_SYSCLK_FREQ / 1000);
        load_val_ = (ticks > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : static_cast<uint32_t>(ticks);
        if (load_val_ == 0)
            load_val_ = 1;

        reg(WDT_LOAD) = load_val_;

        // SP805 / LM3S 硬件规范：
        // INTEN (bit 0) 是递减计数器的主运行使能 (Master Enable)。
        // 若 INTEN=0，计数器根本不会递减。因此 Reset 模式也必须置位 INTEN。
        uint32_t ctl = 0;
        switch (mode) {
        case WatchdogMode::Interrupt:
            ctl = WDT_CTL_INTEN;
            break;
        case WatchdogMode::Reset:
            ctl = WDT_CTL_INTEN | WDT_CTL_RESEN;
            break;
        case WatchdogMode::Both:
            ctl = WDT_CTL_INTEN | WDT_CTL_RESEN;
            break;
        }
        reg(WDT_CTL) = ctl;
        return true;
    }

    void kick() override {
        reg(WDT_LOAD) = load_val_;
    }

    void disable() override {
        reg(WDT_CTL) = 0;
    }

    uint32_t get_remaining() const override {
        return reg(WDT_VALUE);
    }

    bool had_interrupt() const override {
        return interrupt_fired_;
    }

    void handle_interrupt() {
        interrupt_fired_ = true;
        reg(WDT_ICR) = 1; // 写入任意值即可清除 WDT 中断
    }
};

#endif
