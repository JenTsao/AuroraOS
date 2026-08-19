// =============================================================================
// drivers/rf/spectrum_sensor.hpp
//
// 射频频谱传感器抽象层
//
// 定义频谱感知的数据类型与统一驱动接口，屏蔽底层射频前端差异
// （SDR 前端 / 专用频谱芯片 / 收发器 RSSI 通道）。上层分析引擎
// （rf_analyzer.hpp / jamming_detector.hpp）只依赖本抽象，不感知硬件。
//
// 设计原则（遵循 AGENTS.md）：
//   - 功率值采用 Q8 定点表示 (dBm * 256)，全程无浮点依赖
//   - 固定分箱数组，零动态内存分配
//   - 纯虚接口 + 组合，单一职责
//   - MockSpectrumSensor 提供可编程频谱注入，供 host 测试与算法验证
// =============================================================================
#ifndef AURORA_RF_SPECTRUM_SENSOR_HPP
#define AURORA_RF_SPECTRUM_SENSOR_HPP

#include <stdint.h>
#include <stddef.h>

namespace aurora {
namespace rf {

// ---------------------------------------------------------------------------
// 功率定点类型与换算
// ---------------------------------------------------------------------------

// 功率值 Q8 定点：实际 dBm = value / 256。
// int16_t 覆盖约 [-128 dBm, +127.996 dBm]，满足常见射频前端动态范围。
using PowerQ8 = int16_t;

namespace power {
// 整型 dBm -> Q8（编译期常量）
constexpr PowerQ8 from_dbm(int16_t dbm) {
    return static_cast<PowerQ8>(dbm * 256);
}

// Q8 -> 整型 dBm（截断，向零取整）
constexpr int16_t to_dbm(PowerQ8 q8) {
    return static_cast<int16_t>(q8 / 256);
}

// Q8 -> dBm * 10（保留一位小数，避免浮点打印时失真）
constexpr int16_t to_dbm_tenths(PowerQ8 q8) {
    // dBm*10 = value * 10 / 256
    return static_cast<int16_t>((static_cast<int32_t>(q8) * 10) / 256);
}
} // namespace power

// ---------------------------------------------------------------------------
// 频谱分箱上限
// ---------------------------------------------------------------------------
constexpr uint8_t kMaxSpectrumBins = 64;

// 单个频点功率采样（一次扫频结果中的一个分箱）
struct SpectrumBin {
    uint16_t freq_mhz; // 该分箱中心频率 (MHz)
    PowerQ8 power_q8;  // 该频点功率 (dBm, Q8)
};

// 一次完整扫频结果：一段频带内的功率分布
struct SpectrumSweep {
    uint16_t start_freq_mhz; // 扫频起始频率 (MHz)
    uint16_t step_khz;       // 分箱间隔 (kHz)
    uint8_t bin_count;       // 有效分箱数量 (<= kMaxSpectrumBins)
    uint32_t timestamp_ms;   // 采样时间戳 (ms)
    SpectrumBin bins[kMaxSpectrumBins];
};

// ---------------------------------------------------------------------------
// ISpectrumSensor：频谱传感器统一驱动接口
// ---------------------------------------------------------------------------
class ISpectrumSensor {
public:
    virtual ~ISpectrumSensor() = default;

    // 初始化射频前端，进入待机（未上电）状态
    virtual bool init() = 0;

    // 执行一次扫频，将功率分布写入 out，成功返回 true
    virtual bool sweep(SpectrumSweep* out) = 0;

    // 设定扫描频率范围（MHz）
    virtual void set_freq_range(uint16_t start_mhz, uint16_t stop_mhz) = 0;

    // 设定分辨率带宽 / 分箱间隔（kHz）
    virtual void set_resolution_bw(uint16_t step_khz) = 0;

    // 退出休眠，开启射频前端供电
    virtual void power_up() = 0;

    // 进入休眠，切断射频前端供电
    virtual void power_down() = 0;

    // 返回本次配置下的有效分箱数量
    virtual uint8_t get_bin_count() const = 0;
};

// ---------------------------------------------------------------------------
// MockSpectrumSensor：可编程频谱注入（host 测试与算法验证用）
//
// 内部维护一段静态功率表，测试代码通过 fill/inject_* 构造任意频谱，
// sweep() 直接回放当前功率表，不依赖真实射频硬件。
// ---------------------------------------------------------------------------
class MockSpectrumSensor : public ISpectrumSensor {
public:
    MockSpectrumSensor() = default;

    // ---- 配置频率网格 ----
    void configure(uint16_t start_mhz, uint16_t step_khz, uint8_t count) noexcept {
        start_freq_ = start_mhz;
        step_khz_ = step_khz;
        bin_count_ = (count > kMaxSpectrumBins) ? kMaxSpectrumBins : count;
        for (uint8_t i = 0; i < bin_count_; ++i) {
            bins_[i].freq_mhz = static_cast<uint16_t>(start_mhz + (i * step_khz_ / 1000u));
            bins_[i].power_q8 = 0;
        }
    }

    // ---- 功率注入 ----

    // 将所有分箱填充为同一功率（如平坦噪声底）
    void fill(int16_t dbm) noexcept {
        for (uint8_t i = 0; i < bin_count_; ++i) {
            bins_[i].power_q8 = power::from_dbm(dbm);
        }
    }

    // 设置指定分箱功率
    void set_bin(uint8_t index, int16_t dbm) noexcept {
        if (index < bin_count_) {
            bins_[index].power_q8 = power::from_dbm(dbm);
        }
    }

    // 在指定频率注入连续波（自动映射到最近分箱）
    void inject_cw(uint16_t freq_mhz, int16_t dbm) noexcept {
        uint8_t idx = nearest_bin_(freq_mhz);
        if (idx < bin_count_) {
            bins_[idx].power_q8 = power::from_dbm(dbm);
        }
    }

    // 在 [lo_mhz, hi_mhz] 频段内注入统一功率（如窄带/宽带干扰）
    void inject_band(uint16_t lo_mhz, uint16_t hi_mhz, int16_t dbm) noexcept {
        for (uint8_t i = 0; i < bin_count_; ++i) {
            if (bins_[i].freq_mhz >= lo_mhz && bins_[i].freq_mhz <= hi_mhz) {
                bins_[i].power_q8 = power::from_dbm(dbm);
            }
        }
    }

    // 在指定频率上叠加功率（保留原底噪）
    void add_cw(uint16_t freq_mhz, int16_t dbm) noexcept {
        uint8_t idx = nearest_bin_(freq_mhz);
        if (idx < bin_count_) {
            int32_t sum = static_cast<int32_t>(bins_[idx].power_q8) + power::from_dbm(dbm);
            bins_[idx].power_q8 = clamp_q8_(sum);
        }
    }

    // 推进模拟时钟（ms）
    void advance_time(uint32_t ms) noexcept {
        now_ms_ += ms;
    }

    // ---- ISpectrumSensor 实现 ----

    bool init() override {
        powered_ = false;
        return true;
    }

    bool sweep(SpectrumSweep* out) override {
        if (!out || !powered_)
            return false;
        out->start_freq_mhz = start_freq_;
        out->step_khz = step_khz_;
        out->bin_count = bin_count_;
        out->timestamp_ms = now_ms_;
        for (uint8_t i = 0; i < bin_count_; ++i) {
            out->bins[i] = bins_[i];
        }
        return true;
    }

    void set_freq_range(uint16_t start_mhz, uint16_t stop_mhz) override {
        (void)stop_mhz;
        start_freq_ = start_mhz;
    }

    void set_resolution_bw(uint16_t step_khz) override {
        step_khz_ = step_khz;
    }

    void power_up() override {
        powered_ = true;
    }

    void power_down() override {
        powered_ = false;
    }

    uint8_t get_bin_count() const override {
        return bin_count_;
    }

private:
    SpectrumBin bins_[kMaxSpectrumBins]{};
    uint8_t bin_count_ = 0;
    uint16_t start_freq_ = 2400;
    uint16_t step_khz_ = 1000;
    uint32_t now_ms_ = 0;
    bool powered_ = false;

    uint8_t nearest_bin_(uint16_t freq_mhz) const noexcept {
        if (bin_count_ == 0)
            return 0;
        uint8_t best = 0;
        int32_t best_dist = 0x7FFFFFFF;
        for (uint8_t i = 0; i < bin_count_; ++i) {
            int32_t dist = static_cast<int32_t>(freq_mhz) - static_cast<int32_t>(bins_[i].freq_mhz);
            if (dist < 0)
                dist = -dist;
            if (dist < best_dist) {
                best_dist = dist;
                best = i;
            }
        }
        return best;
    }

    static PowerQ8 clamp_q8_(int32_t v) noexcept {
        if (v > 32767)
            return 32767;
        if (v < -32768)
            return -32768;
        return static_cast<PowerQ8>(v);
    }
};

} // namespace rf
} // namespace aurora

#endif // AURORA_RF_SPECTRUM_SENSOR_HPP
