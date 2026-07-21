#ifndef LM3S_WDT_HPP
#define LM3S_WDT_HPP

#include "watchdog.hpp"
#include "board.h"

// TI LM3S6965 Watchdog Timer 寄存器 (StellarisWare)
class Lm3sWdt : public WatchdogDriver {
private:
    static constexpr uintptr_t WDT_LOAD  = BOARD_WDT_BASE + 0x000;
    static constexpr uintptr_t WDT_VALUE = BOARD_WDT_BASE + 0x004;
    static constexpr uintptr_t WDT_CTL   = BOARD_WDT_BASE + 0x008;
    static constexpr uintptr_t WDT_ICR   = BOARD_WDT_BASE + 0x00C;
    static constexpr uintptr_t WDT_RIS   = BOARD_WDT_BASE + 0x010;
    static constexpr uintptr_t WDT_MIS   = BOARD_WDT_BASE + 0x014;

    static constexpr uint32_t WDT_CTL_INTEN = (1UL << 0);
    static constexpr uint32_t WDT_CTL_RESEN = (1UL << 1);
    static constexpr uint32_t WDT_ICR_UNLOCK = 0x1ACCE551U;

    uint32_t load_val_ = 0;
    bool interrupt_fired_ = false;

    static volatile uint32_t& reg(uint32_t offset) {
        return *reinterpret_cast<volatile uint32_t*>(offset);
    }

public:
    bool init(uint32_t timeout_ms, WatchdogMode mode = WatchdogMode::Reset) override {
        interrupt_fired_ = false;

        // load = timeout_ms * (SYSCLK / 1000)
        load_val_ = timeout_ms * (BOARD_SYSCLK_FREQ / 1000);
        if (load_val_ == 0) load_val_ = 1;

        reg(WDT_LOAD) = load_val_;

        uint32_t ctl = 0;
        switch (mode) {
            case WatchdogMode::Interrupt: ctl = WDT_CTL_INTEN; break;
            case WatchdogMode::Reset:     ctl = WDT_CTL_RESEN; break;
            case WatchdogMode::Both:      ctl = WDT_CTL_INTEN | WDT_CTL_RESEN; break;
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
        reg(WDT_ICR) = WDT_ICR_UNLOCK;
    }
};

#endif
