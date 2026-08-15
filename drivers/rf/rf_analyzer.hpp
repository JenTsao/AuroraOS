// =============================================================================
// drivers/rf/rf_analyzer.hpp
//
// 射频频谱异常信号检测引擎
//
// 消费 ISpectrumSensor 产生的 SpectrumSweep，逐分箱维护噪声底估计
// （指数滑动平均 EMA + 绝对偏差），并在以下维度检测异常：
//   - AboveNoiseFloor：功率显著高于该分箱历史噪声底
//   - AbsoluteHigh   ：功率超过绝对安全上限（饱和/强干扰）
//   - SuddenBurst    ：相邻两次采样间功率发生瞬时跳变（突发信号）
//   - WidebandRise   ：大量分箱同时抬升（宽带压制 / 底噪整体抬高）
//
// 设计原则（遵循 AGENTS.md）：
//   - 全定点 Q8 运算，无浮点、无开方（用绝对偏差代替标准差）
//   - 固定数组，零动态内存分配
//   - 纯计算无锁：由调用方（频谱守护任务）单线程推进，与 health_algo 一致
//   - 与硬件解耦：只依赖 spectrum_sensor.hpp 的数据类型
// =============================================================================
#ifndef AURORA_RF_ANALYZER_HPP
#define AURORA_RF_ANALYZER_HPP

#include <stdint.h>
#include "spectrum_sensor.hpp"

namespace aurora {
namespace rf {

// ---------------------------------------------------------------------------
// 异常类型
// ---------------------------------------------------------------------------
enum class AnomalyType : uint8_t {
    None = 0,
    AboveNoiseFloor, // 超过本地噪声底
    AbsoluteHigh,    // 超过绝对上限
    SuddenBurst,     // 瞬时突发跳变
    WidebandRise,    // 宽带底噪整体抬升
};

// ---------------------------------------------------------------------------
// 异常事件
// ---------------------------------------------------------------------------
struct AnomalyEvent {
    uint32_t timestamp_ms;   // 时间戳
    uint16_t freq_mhz;       // 触发分箱中心频率
    uint8_t bin_index;       // 触发分箱索引
    PowerQ8 power_q8;        // 当前功率
    PowerQ8 baseline_q8;     // 该分箱噪声底（WidebandRise 时为全频带均值）
    AnomalyType type;        // 异常类型
    uint8_t severity;        // 严重度 0-100
};

// ---------------------------------------------------------------------------
// RfAnalyzer：频谱异常检测引擎
// ---------------------------------------------------------------------------
class RfAnalyzer {
public:
    // ---- 配置（均为定点 dBm）----

    // 相对噪声底的检测阈值（默认 +6 dBm）
    void set_noise_margin_dbm(int16_t dbm) noexcept {
        noise_margin_q8_ = power::from_dbm(dbm);
    }

    // 绝对功率上限（默认 -20 dBm，超过即 AbsoluteHigh）
    void set_absolute_max_dbm(int16_t dbm) noexcept {
        absolute_max_q8_ = power::from_dbm(dbm);
    }

    // 突发检测的瞬时跳变阈值（默认 +10 dBm）
    void set_burst_delta_dbm(int16_t dbm) noexcept {
        burst_delta_q8_ = power::from_dbm(dbm);
    }

    // 宽带抬升判定：活动分箱占比阈值（百分比 0-100，默认 60）
    void set_wideband_ratio(uint8_t percent) noexcept {
        wideband_ratio_ = (percent > 100) ? 100 : percent;
    }

    // ---- 分析 ----

    // 处理一次扫频，将检测到的异常写入 out（最多 max 条），返回异常数量。
    // out 可为 nullptr 且 max 可为 0：此时仅推进噪声底基线，不输出事件。
    // 调用期间同步更新每个分箱的噪声底估计。
    int analyze(const SpectrumSweep& sweep, AnomalyEvent* out, int max) {
        const uint8_t count = (sweep.bin_count > kMaxSpectrumBins) ? kMaxSpectrumBins : sweep.bin_count;
        int emitted = 0;

        // 第一遍：更新噪声底基线，逐分箱记录异常类型，统计活动分箱
        uint8_t active_bins = 0;
        int32_t band_power_sum = 0;
        AnomalyType bin_types[kMaxSpectrumBins];
        PowerQ8 bin_threshold[kMaxSpectrumBins];

        for (uint8_t i = 0; i < count; ++i) {
            const int32_t sample = sweep.bins[i].power_q8;

            // 1. 噪声底 EMA 更新（首帧直接取样本为基线）
            if (!initialized_) {
                floor_q8_[i] = static_cast<PowerQ8>(sample);
                dev_q8_[i] = 0;
            } else {
                floor_q8_[i] = static_cast<PowerQ8>(floor_q8_[i] + ((sample - floor_q8_[i]) * kFloorAlphaNum) / kFloorAlphaDen);
                int32_t abs_err = sample - floor_q8_[i];
                if (abs_err < 0)
                    abs_err = -abs_err;
                dev_q8_[i] = static_cast<PowerQ8>(dev_q8_[i] + ((abs_err - dev_q8_[i]) * kDevBetaNum) / kDevBetaDen);
            }

            band_power_sum += sample;
            const int32_t threshold_q8 = floor_q8_[i] + noise_margin_q8_ + dev_q8_[i];
            bin_threshold[i] = static_cast<PowerQ8>(threshold_q8);

            // 2. 分箱级异常判定（优先级：绝对上限 > 瞬时突发 > 持续超噪声底）
            AnomalyType t = AnomalyType::None;
            if (sample > absolute_max_q8_) {
                t = AnomalyType::AbsoluteHigh;
            } else if (initialized_) {
                int32_t delta = sample - prev_power_q8_[i];
                if (delta < 0)
                    delta = -delta;
                if (delta > burst_delta_q8_) {
                    t = AnomalyType::SuddenBurst;
                } else if (sample > threshold_q8) {
                    t = AnomalyType::AboveNoiseFloor;
                }
            }

            bin_types[i] = t;
            if (t != AnomalyType::None && initialized_) {
                ++active_bins;
            }
            prev_power_q8_[i] = static_cast<PowerQ8>(sample);
        }

        // 3. 输出：宽带压制优先输出全局告警，抑制 bin 级噪音
        const bool wideband = initialized_ && count > 0 &&
                              static_cast<uint8_t>((active_bins * 100u) / count) >= wideband_ratio_;

        if (wideband) {
            if (out != nullptr && max > 0) {
                AnomalyEvent& ev = out[emitted++];
                ev.timestamp_ms = sweep.timestamp_ms;
                ev.freq_mhz = sweep.start_freq_mhz;
                ev.bin_index = 0;
                ev.power_q8 = static_cast<PowerQ8>(band_power_sum / count);
                ev.baseline_q8 = band_mean_floor_(count);
                ev.type = AnomalyType::WidebandRise;
                ev.severity = 85;
            }
        } else {
            for (uint8_t i = 0; i < count && out != nullptr && emitted < max; ++i) {
                if (bin_types[i] == AnomalyType::None)
                    continue;
                AnomalyEvent& ev = out[emitted++];
                ev.timestamp_ms = sweep.timestamp_ms;
                ev.freq_mhz = sweep.bins[i].freq_mhz;
                ev.bin_index = i;
                ev.power_q8 = sweep.bins[i].power_q8;
                ev.baseline_q8 = floor_q8_[i];
                ev.type = bin_types[i];
                ev.severity = severity_of_(bin_types[i], sweep.bins[i].power_q8, bin_threshold[i]);
            }
        }

        initialized_ = true;
        return emitted;
    }

    // ---- 噪声底查询 ----

    // 返回指定分箱的噪声底（Q8）。未初始化时返回 0。
    PowerQ8 get_noise_floor(uint8_t bin_index) const noexcept {
        if (bin_index >= kMaxSpectrumBins)
            return 0;
        return floor_q8_[bin_index];
    }

    // 判定分箱当前是否“活动”（功率高于噪声底 + 阈值）
    bool is_active_bin(const SpectrumSweep& sweep, uint8_t bin_index) const noexcept {
        if (bin_index >= sweep.bin_count || bin_index >= kMaxSpectrumBins)
            return false;
        const int32_t threshold = static_cast<int32_t>(floor_q8_[bin_index]) + noise_margin_q8_ + dev_q8_[bin_index];
        return sweep.bins[bin_index].power_q8 > threshold;
    }

    // 是否已建立基线
    bool is_calibrated() const noexcept {
        return initialized_;
    }

    // 重置全部状态（重新建基线）
    void reset() noexcept {
        initialized_ = false;
        for (uint8_t i = 0; i < kMaxSpectrumBins; ++i) {
            floor_q8_[i] = 0;
            dev_q8_[i] = 0;
            prev_power_q8_[i] = 0;
        }
    }

    // ---- 工具 ----

    static const char* type_name(AnomalyType t) noexcept {
        switch (t) {
        case AnomalyType::AboveNoiseFloor:
            return "above_noise_floor";
        case AnomalyType::AbsoluteHigh:
            return "absolute_high";
        case AnomalyType::SuddenBurst:
            return "sudden_burst";
        case AnomalyType::WidebandRise:
            return "wideband_rise";
        default:
            return "none";
        }
    }

private:
    // 噪声底 EMA 系数：alpha = 1/16
    static constexpr int32_t kFloorAlphaNum = 1;
    static constexpr int32_t kFloorAlphaDen = 16;
    // 偏差 EMA 系数：beta = 1/8
    static constexpr int32_t kDevBetaNum = 1;
    static constexpr int32_t kDevBetaDen = 8;

    PowerQ8 floor_q8_[kMaxSpectrumBins]{};
    PowerQ8 dev_q8_[kMaxSpectrumBins]{};
    PowerQ8 prev_power_q8_[kMaxSpectrumBins]{};
    bool initialized_ = false;

    PowerQ8 noise_margin_q8_ = power::from_dbm(6);    // 相对底噪 +6 dBm
    PowerQ8 absolute_max_q8_ = power::from_dbm(-20);  // 绝对上限 -20 dBm
    PowerQ8 burst_delta_q8_ = power::from_dbm(10);    // 突发跳变 +10 dBm
    uint8_t wideband_ratio_ = 60;                     // 宽带判定占比 60%

    // 计算全频带噪声底均值（用于 WidebandRise 基线参考）
    PowerQ8 band_mean_floor_(uint8_t count) const noexcept {
        if (count == 0)
            return 0;
        int32_t sum = 0;
        for (uint8_t i = 0; i < count; ++i) {
            sum += floor_q8_[i];
        }
        return static_cast<PowerQ8>(sum / count);
    }

    // 严重度评分：超出越多、类型越危险，分值越高
    static uint8_t severity_of_(AnomalyType t, PowerQ8 sample, int32_t threshold_q8) noexcept {
        int32_t excess = static_cast<int32_t>(sample) - threshold_q8; // Q8
        if (excess < 0)
            excess = 0;
        int32_t excess_dbm = excess / 256;

        uint8_t base;
        switch (t) {
        case AnomalyType::AbsoluteHigh:
            base = 90;
            break;
        case AnomalyType::SuddenBurst:
            base = 70;
            break;
        case AnomalyType::AboveNoiseFloor:
            base = 50;
            break;
        default:
            base = 30;
            break;
        }

        // 每超出 1 dBm 加 2 分，封顶 100
        int32_t score = base + excess_dbm * 2;
        if (score > 100)
            score = 100;
        return static_cast<uint8_t>(score);
    }
};

} // namespace rf
} // namespace aurora

#endif // AURORA_RF_ANALYZER_HPP
