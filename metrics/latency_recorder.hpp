#ifndef AURORA_LATENCY_RECORDER_HPP
#define AURORA_LATENCY_RECORDER_HPP

#include <stdint.h>
#include "arch_api.hpp"

class LatencyRecorder {
private:
    uint32_t count_;
    uint64_t total_cycles_;
    uint32_t max_cycles_;
    uint32_t min_cycles_;
    /* History size: full 100 for lm3s/rv32; configurable for small RAM targets
     * via AURORA_METRICS_HIST_SIZE (default 100 if not defined). */
#ifndef AURORA_METRICS_HIST_SIZE
    static constexpr int HIST_SIZE = 100;
#else
    static constexpr int HIST_SIZE = AURORA_METRICS_HIST_SIZE;
#endif
    uint32_t history_[HIST_SIZE];
    uint32_t hist_idx_;

public:
    LatencyRecorder() {
        reset();
    }

    void reset() {
        count_ = 0;
        total_cycles_ = 0;
        max_cycles_ = 0;
        min_cycles_ = 0xFFFFFFFF;
        hist_idx_ = 0;
        for (int i = 0; i < HIST_SIZE; i++)
            history_[i] = 0;
    }

    void record(uint32_t cycles) {
        count_++;
        total_cycles_ += cycles;
        if (cycles > max_cycles_)
            max_cycles_ = cycles;
        if (cycles < min_cycles_)
            min_cycles_ = cycles;

        history_[hist_idx_] = cycles;
        hist_idx_ = (hist_idx_ + 1) % HIST_SIZE;
    }

    uint32_t get_avg_us() const {
        if (count_ == 0)
            return 0;
        uint32_t cpu = Arch::get_cycles_per_us();
        if (cpu == 0)
            return 0;
        uint32_t avg_cycles = total_cycles_ / count_;
        return avg_cycles / cpu;
    }

    uint32_t get_avg_cycles() const {
        if (count_ == 0)
            return 0;
        return static_cast<uint32_t>(total_cycles_ / count_);
    }

    uint32_t get_max_us() const {
        if (count_ == 0)
            return 0;
        uint32_t cpu = Arch::get_cycles_per_us();
        if (cpu == 0)
            return 0;
        return max_cycles_ / cpu;
    }

    uint32_t get_min_us() const {
        if (count_ == 0)
            return 0;
        uint32_t cpu = Arch::get_cycles_per_us();
        if (cpu == 0)
            return 0;
        return min_cycles_ / cpu;
    }

    uint32_t get_min_cycles() const {
        if (count_ == 0)
            return 0;
        return min_cycles_;
    }

    // Note: get_p99_us only considers the most recent HIST_SIZE samples.
    // temp[] is sized to HIST_SIZE (100 for lm3s/rv32, 16 for nucleo).
    // On nucleo this is just 64 bytes of shell-stack space; on lm3s it's
    // 400 bytes which is fine with the 64KB RAM target.
    uint32_t get_p99_us() const {
        if (count_ == 0)
            return 0;
        uint32_t cpu = Arch::get_cycles_per_us();
        if (cpu == 0)
            return 0;
        int valid_samples = count_ < HIST_SIZE ? count_ : HIST_SIZE;
        uint32_t temp[HIST_SIZE];
        for (int i = 0; i < valid_samples; i++)
            temp[i] = history_[i];

        for (int i = 0; i < valid_samples - 1; i++) {
            for (int j = 0; j < valid_samples - i - 1; j++) {
                if (temp[j] > temp[j + 1]) {
                    uint32_t t = temp[j];
                    temp[j] = temp[j + 1];
                    temp[j + 1] = t;
                }
            }
        }

        int p99_idx = (valid_samples * 99) / 100;
        if (p99_idx >= valid_samples)
            p99_idx = valid_samples - 1;
        return temp[p99_idx] / cpu;
    }

    uint32_t get_count() const {
        return count_;
    }
};

#endif
