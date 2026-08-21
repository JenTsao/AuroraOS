#ifndef AURORA_POWER_PROFILER_HPP
#define AURORA_POWER_PROFILER_HPP

#include <stdint.h>
#include "arch_api.hpp"

class PowerProfiler {
private:
    uint64_t total_sleep_cycles_;
    uint64_t total_active_cycles_;
    uint32_t sleep_count_;
    uint32_t active_ms_;
    uint32_t dim_ms_;
    uint32_t idle_ms_;
    uint32_t sleep_ms_;

public:
    PowerProfiler() {
        reset();
    }

    void reset() {
        total_sleep_cycles_ = 0;
        total_active_cycles_ = 0;
        sleep_count_ = 0;
        active_ms_ = 0;
        dim_ms_ = 0;
        idle_ms_ = 0;
        sleep_ms_ = 0;
    }

    void add_sleep_time(uint32_t sleep_cycles) {
        total_sleep_cycles_ += sleep_cycles;
        sleep_count_++;
    }

    void update_active_time(uint32_t cycles) {
        total_active_cycles_ += cycles;
    }

    void record_state_duration(uint8_t state, uint32_t duration_ms) {
        switch (state) {
        case 0: active_ms_ += duration_ms; break; // ACTIVE
        case 1: dim_ms_ += duration_ms; break;    // DIM
        case 2: idle_ms_ += duration_ms; break;   // IDLE
        default: sleep_ms_ += duration_ms; break; // SLEEP / CRITICAL
        }
    }

    uint32_t get_sleep_ratio() const {
        uint64_t total = total_sleep_cycles_ + total_active_cycles_;
        if (total == 0)
            return 0;
        return (total_sleep_cycles_ * 100) / total;
    }

    uint32_t get_sleep_count() const {
        return sleep_count_;
    }

    // 估算平均功耗电流 (单位: uA)
    // 典型功耗模型: Active ~15000uA, Dim ~8000uA, Idle ~1000uA, Sleep ~100uA
    uint32_t calculate_average_current_ua() const {
        uint64_t total_time_ms = active_ms_ + dim_ms_ + idle_ms_ + sleep_ms_;
        if (total_time_ms == 0) return 0;
        uint64_t total_ua_ms = (uint64_t)active_ms_ * 15000 +
                               (uint64_t)dim_ms_ * 8000 +
                               (uint64_t)idle_ms_ * 1000 +
                               (uint64_t)sleep_ms_ * 100;
        return static_cast<uint32_t>(total_ua_ms / total_time_ms);
    }
};

#endif

