#ifndef AURORA_POWER_PROFILER_HPP
#define AURORA_POWER_PROFILER_HPP

#include <stdint.h>
#include "arch_api.hpp"

class PowerProfiler {
private:
    uint64_t total_sleep_cycles_;
    uint64_t total_active_cycles_;
    uint32_t sleep_count_;

public:
    PowerProfiler() { reset(); }

    void reset() {
        total_sleep_cycles_ = 0;
        total_active_cycles_ = 0;
        sleep_count_ = 0;
    }

    void add_sleep_time(uint32_t sleep_cycles) {
        total_sleep_cycles_ += sleep_cycles;
        sleep_count_++;
    }
    
    void update_active_time(uint32_t cycles) {
        total_active_cycles_ += cycles;
    }

    uint32_t get_sleep_ratio() const {
        uint64_t total = total_sleep_cycles_ + total_active_cycles_;
        if (total == 0) return 0;
        return (total_sleep_cycles_ * 100) / total;
    }
    
    uint32_t get_sleep_count() const { return sleep_count_; }
};

#endif
