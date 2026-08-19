// =============================================================================
// drivers/rf/jamming_detector.hpp
//
// 射频干扰信号识别引擎
//
// 消费 SpectrumSweep（并复用 RfAnalyzer 的噪声底估计）跨帧识别物理层干扰：
//   - ContinuousWave ：单频点持续高功率（窄带连续波压制）
//   - Narrowband     ：少量分箱高功率（局部窄带干扰）
//   - BroadbandNoise ：全频带功率均匀抬升（宽带噪声压制）
//   - SweepingChirp  ：高功率峰值频率随时间线性移动（扫频干扰）
//   - Pulsed         ：高功率间歇性出现（脉冲/猝发干扰）
//   - Spoofing       ：疑似协议感知干扰（保留类型，需协议层协同确认）
//
// 设计原则（遵循 AGENTS.md）：
//   - 组合优于继承：持有 RfAnalyzer 引用，复用其噪声底与活动分箱判定
//   - 固定环形历史缓冲，零动态内存分配
//   - 全定点运算、noexcept
//   - 纯计算无锁：由调用方（频谱守护任务）单线程推进
//   - 检测结果由调用方决定是否上报 SecurityMonitor（模块自身不耦合内核）
// =============================================================================
#ifndef AURORA_RF_JAMMING_DETECTOR_HPP
#define AURORA_RF_JAMMING_DETECTOR_HPP

#include <stdint.h>
#include "spectrum_sensor.hpp"
#include "rf_analyzer.hpp"

namespace aurora {
namespace rf {

// ---------------------------------------------------------------------------
// 干扰类型
// ---------------------------------------------------------------------------
enum class JammingType : uint8_t {
    None = 0,
    ContinuousWave, // 连续波干扰
    Narrowband,     // 窄带干扰
    BroadbandNoise, // 宽带噪声压制
    SweepingChirp,  // 扫频干扰
    Pulsed,         // 脉冲干扰
    Spoofing,       // 疑似协议感知欺骗干扰
};

// ---------------------------------------------------------------------------
// 干扰告警
// ---------------------------------------------------------------------------
struct JammingAlert {
    uint32_t timestamp_ms;   // 时间戳
    JammingType type;        // 干扰类型
    uint16_t center_freq_mhz; // 干扰中心频率
    uint16_t bandwidth_khz;   // 干扰带宽估算
    uint8_t severity;         // 严重度 0-100
    uint8_t confidence;       // 置信度 0-100
    char description[64];     // 可读描述
};

// ---------------------------------------------------------------------------
// JammingDetector：跨帧干扰识别
// ---------------------------------------------------------------------------
class JammingDetector {
public:
    // 绑定一个频谱分析器（用于活动分箱判定与噪声底估计）
    explicit JammingDetector(RfAnalyzer& analyzer) noexcept : analyzer_(analyzer) {}

    // ---- 配置 ----

    // 宽带压制判定：活动分箱占比阈值（百分比 0-100，默认 60）
    void set_wideband_ratio(uint8_t percent) noexcept {
        wideband_ratio_ = (percent > 100) ? 100 : percent;
    }

    // 连续波判定：峰值频率稳定在 1 个分箱步进内的连续帧数（默认 3）
    void set_cw_confirm_frames(uint8_t frames) noexcept {
        cw_confirm_frames_ = (frames < 2) ? 2 : frames;
    }

    // 扫频判定：峰值频率单调移动的连续帧数（默认 3）
    void set_sweep_confirm_frames(uint8_t frames) noexcept {
        sweep_confirm_frames_ = (frames < 2) ? 2 : frames;
    }

    // ---- 检测 ----

    // 处理一次扫频，若识别到干扰则返回告警指针（内部静态缓冲，
    // 调用方应立即消费；无干扰返回 nullptr）。同时推进分析器基线。
    const JammingAlert* detect(const SpectrumSweep& sweep) noexcept {
        const uint8_t count = (sweep.bin_count > kMaxSpectrumBins) ? kMaxSpectrumBins : sweep.bin_count;
        if (count == 0)
            return nullptr;

        // 1. 推进分析器基线（不输出异常事件）
        analyzer_.analyze(sweep, nullptr, 0);

        // 2. 统计活动分箱 + 定位峰值 + 估算活动带宽
        uint8_t active_count = 0;
        uint8_t peak_idx = 0;
        int32_t peak_power = -32768;
        uint16_t lo_freq = 0;
        uint16_t hi_freq = 0;
        bool has_active = false;

        for (uint8_t i = 0; i < count; ++i) {
            const bool active = analyzer_.is_active_bin(sweep, i);
            if (active) {
                ++active_count;
                if (!has_active) {
                    lo_freq = sweep.bins[i].freq_mhz;
                    hi_freq = sweep.bins[i].freq_mhz;
                    has_active = true;
                } else {
                    if (sweep.bins[i].freq_mhz < lo_freq)
                        lo_freq = sweep.bins[i].freq_mhz;
                    if (sweep.bins[i].freq_mhz > hi_freq)
                        hi_freq = sweep.bins[i].freq_mhz;
                }
            }
            if (sweep.bins[i].power_q8 > peak_power) {
                peak_power = sweep.bins[i].power_q8;
                peak_idx = i;
            }
        }

        const uint16_t peak_freq = sweep.bins[peak_idx].freq_mhz;

        // 3. 记录峰值历史（环形缓冲）
        history_[hist_head_].freq_mhz = peak_freq;
        history_[hist_head_].power_q8 = static_cast<PowerQ8>(peak_power);
        history_[hist_head_].active_count = active_count;
        history_[hist_head_].timestamp_ms = sweep.timestamp_ms;
        hist_head_ = (hist_head_ + 1) % kMaxHistory;
        if (hist_count_ < kMaxHistory)
            ++hist_count_;

        // 4. 无活动：无干扰
        if (active_count == 0) {
            return nullptr;
        }

        // 5. 分类
        JammingType type = classify_(active_count, count, sweep.step_khz);
        if (type == JammingType::None)
            return nullptr;

        // 6. 填充告警
        static JammingAlert alert;
        alert.timestamp_ms = sweep.timestamp_ms;
        alert.type = type;
        // 中心频率取活动频带中心（单分箱时即为峰值频率）
        alert.center_freq_mhz = static_cast<uint16_t>((lo_freq + hi_freq) / 2u);
        // 带宽估算：单分箱按 1 个 step，多分箱按活动跨度 + 1 个 step
        if (active_count <= 1) {
            alert.bandwidth_khz = sweep.step_khz;
        } else {
            alert.bandwidth_khz = static_cast<uint16_t>(hi_freq - lo_freq) * 1000u + sweep.step_khz;
        }
        alert.severity = severity_of_(type);
        alert.confidence = confidence_of_(type);
        fill_description_(alert, type, peak_freq);

        return &alert;
    }

    // ---- 统计 ----

    uint32_t get_detection_count() const noexcept {
        return detection_count_;
    }

    // 重置历史与统计（噪声底由 RfAnalyzer::reset 独立控制）
    void reset() noexcept {
        hist_head_ = 0;
        hist_count_ = 0;
        detection_count_ = 0;
    }

    // ---- 工具 ----

    static const char* type_name(JammingType t) noexcept {
        switch (t) {
        case JammingType::ContinuousWave:
            return "continuous_wave";
        case JammingType::Narrowband:
            return "narrowband";
        case JammingType::BroadbandNoise:
            return "broadband_noise";
        case JammingType::SweepingChirp:
            return "sweeping_chirp";
        case JammingType::Pulsed:
            return "pulsed";
        case JammingType::Spoofing:
            return "spoofing";
        default:
            return "none";
        }
    }

private:
    static constexpr uint8_t kMaxHistory = 16;

    // 峰值历史记录（跨帧分类依据）
    struct PeakRecord {
        uint16_t freq_mhz;
        PowerQ8 power_q8;
        uint8_t active_count; // 0 = 该帧无活动
        uint32_t timestamp_ms;
    };

    RfAnalyzer& analyzer_;

    PeakRecord history_[kMaxHistory]{};
    uint8_t hist_head_ = 0;
    uint8_t hist_count_ = 0;

    uint8_t wideband_ratio_ = 60;     // 宽带占比阈值
    uint8_t cw_confirm_frames_ = 3;   // CW 确认帧数
    uint8_t sweep_confirm_frames_ = 3; // 扫频确认帧数
    uint32_t detection_count_ = 0;

    // 访问第 idx 近的历史帧（0 = 最旧，hist_count_-1 = 最新）
    const PeakRecord& hist_at_(uint8_t idx) const noexcept {
        const int real = (hist_head_ - hist_count_ + idx + kMaxHistory * 2) % kMaxHistory;
        return history_[real];
    }

    // ---- 分类核心 ----

    JammingType classify_(uint8_t active_count, uint8_t total, uint16_t step_khz) noexcept {
        // 宽带噪声：活动分箱占比超阈值
        const uint8_t ratio = static_cast<uint8_t>((active_count * 100u) / total);
        if (ratio >= wideband_ratio_) {
            ++detection_count_;
            return JammingType::BroadbandNoise;
        }

        // 局部干扰进一步判别（需要足够历史帧）
        if (is_cw_(step_khz))
            return JammingType::ContinuousWave;
        if (is_sweeping_(step_khz))
            return JammingType::SweepingChirp;
        if (is_pulsed_())
            return JammingType::Pulsed;

        // 默认：局部窄带（单帧或未满足以上模式）
        ++detection_count_;
        return JammingType::Narrowband;
    }

    // 连续波：最近 N 帧峰值频率稳定在同一分箱内
    bool is_cw_(uint16_t step_khz) noexcept {
        const uint8_t need = cw_confirm_frames_;
        if (hist_count_ < need)
            return false;

        // 从最新往前取 need 帧，检查是否有活动且频率一致
        const uint8_t latest = hist_count_ - 1;
        const PeakRecord& base = hist_at_(latest);
        if (base.active_count == 0)
            return false;

        uint8_t matched = 1;
        for (uint8_t k = 1; k < need; ++k) {
            const PeakRecord& rec = hist_at_(static_cast<uint8_t>(latest - k));
            if (rec.active_count == 0)
                return false; // 期间有空帧，不是持续 CW
            int32_t df = static_cast<int32_t>(rec.freq_mhz) - static_cast<int32_t>(base.freq_mhz);
            if (df < 0)
                df = -df;
            // 频率漂移在 1 个分箱步进内视为稳定
            if (df * 1000 > static_cast<int32_t>(step_khz))
                return false;
            ++matched;
        }
        if (matched >= need)
            ++detection_count_;
        return matched >= need;
    }

    // 扫频：最近 N 帧峰值频率单调递增或递减
    bool is_sweeping_(uint16_t step_khz) noexcept {
        const uint8_t need = sweep_confirm_frames_;
        if (hist_count_ < need)
            return false;

        const uint8_t latest = hist_count_ - 1;
        const PeakRecord& newest = hist_at_(latest);
        if (newest.active_count == 0)
            return false;

        // 比较相邻帧频率，判断是否每帧都移动且方向一致
        bool rising = false;
        bool falling = false;
        uint8_t moves = 0;

        for (uint8_t k = 1; k < need; ++k) {
            const PeakRecord& cur = hist_at_(static_cast<uint8_t>(latest - k + 1));
            const PeakRecord& prev = hist_at_(static_cast<uint8_t>(latest - k));
            if (cur.active_count == 0 || prev.active_count == 0)
                return false; // 扫频期间不允许空帧

            // freq_mhz 差值单位是 MHz，需换算为 kHz 后与 step_khz 比较
            const int32_t df_khz =
                (static_cast<int32_t>(cur.freq_mhz) - static_cast<int32_t>(prev.freq_mhz)) * 1000;
            const int32_t one_bin_khz = static_cast<int32_t>(step_khz);
            if (df_khz > one_bin_khz) {
                rising = true;
                ++moves;
            } else if (df_khz < -one_bin_khz) {
                falling = true;
                ++moves;
            }
            // 移动不足 1 bin 不计入扫频
        }

        // 方向一致且确实有移动
        if (rising == falling)
            return false;
        if (moves == 0)
            return false;

        ++detection_count_;
        return true;
    }

    // 脉冲：历史中存在活动帧与空帧交替
    bool is_pulsed_() noexcept {
        if (hist_count_ < 3)
            return false;

        bool seen_active = false;
        bool seen_gap = false;
        bool seen_active_after_gap = false;

        for (uint8_t i = 0; i < hist_count_; ++i) {
            const PeakRecord& rec = hist_at_(i);
            if (rec.active_count > 0) {
                if (seen_gap) {
                    seen_active_after_gap = true;
                } else {
                    seen_active = true;
                }
            } else {
                if (seen_active) {
                    seen_gap = true;
                }
            }
        }

        if (seen_active_after_gap)
            ++detection_count_;
        return seen_active_after_gap;
    }

    // ---- 评分 ----

    static uint8_t severity_of_(JammingType t) noexcept {
        switch (t) {
        case JammingType::BroadbandNoise:
            return 85;
        case JammingType::SweepingChirp:
            return 75;
        case JammingType::ContinuousWave:
            return 70;
        case JammingType::Pulsed:
            return 70;
        case JammingType::Spoofing:
            return 80;
        case JammingType::Narrowband:
            return 65;
        default:
            return 0;
        }
    }

    static uint8_t confidence_of_(JammingType t) noexcept {
        switch (t) {
        case JammingType::BroadbandNoise:
            return 90;
        case JammingType::SweepingChirp:
            return 80;
        case JammingType::ContinuousWave:
            return 85;
        case JammingType::Pulsed:
            return 70;
        case JammingType::Narrowband:
            return 60;
        case JammingType::Spoofing:
            return 40;
        default:
            return 0;
        }
    }

    // 填充可读描述（零堆分配，手动十进制/拼接）
    void fill_description_(JammingAlert& alert, JammingType type, uint16_t freq_mhz) noexcept {
        char* p = alert.description;
        const char* const end = alert.description + sizeof(alert.description) - 1;

        auto app_s = [&](const char* s) {
            while (*s && p < end)
                *p++ = *s++;
        };
        auto app_u = [&](uint32_t v) {
            char digits[8];
            int n = 0;
            if (v == 0) {
                digits[n++] = '0';
            }
            while (v > 0 && n < 8) {
                digits[n++] = static_cast<char>('0' + (v % 10u));
                v /= 10u;
            }
            while (n > 0 && p < end)
                *p++ = digits[--n];
        };

        app_s("RF jamming: ");
        app_s(type_name(type));
        app_s(" @ ");
        app_u(freq_mhz);
        app_s(" MHz");
        *p = '\0';
    }
};

} // namespace rf
} // namespace aurora

#endif // AURORA_RF_JAMMING_DETECTOR_HPP
