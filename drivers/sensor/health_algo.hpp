#ifndef AURORA_HEALTH_ALGO_HPP
#define AURORA_HEALTH_ALGO_HPP

// ============================================================
// health_algo.hpp — Aurora OS 运动健康算法框架
//
// 设计原则 (cpp-coding-standards):
//   - RAII / Rule of Zero: 全部值类型成员，无裸指针、无 new/delete
//   - const-by-default: 所有只读方法标注 const noexcept
//   - enum class: 无作用域污染的强类型枚举
//   - noexcept: 纯计算函数保证不抛异常
//   - constexpr: 编译期常量，无魔法数字 (ES.45)
//   - namespace aurora::health: 隔离算法命名空间
//   - 无 I/O、无全局状态：可直接在主机 GTest 中注入测试数据 (cpp-testing DI)
// ============================================================

#include <stdint.h>
#include <stddef.h>

namespace aurora {
namespace health {

// ============================================================
// ActivityState — 活动状态枚举 (Enum.3: enum class)
// ============================================================
enum class ActivityState : uint8_t {
    unknown,   // 初始化中，数据不足以判断
    still,     // 静止 >= 30 秒
    walking,   // 步频 60~129 步/分钟
    running,   // 步频 >= 130 步/分钟
    sleeping   // 静止 >= 5 分钟 且 心率 < 65 BPM
};

// ============================================================
// 1. PpgHeartRateFilter — 滑动窗口 + IIR 低通滤波器
//    输入：原始 PPG 心率采样 (BPM)
//    输出：去噪后的稳定 BPM 整数值
//
// 实现：
//   - 阶段一：长度 8 的滑动均值窗口消除瞬时尖峰 (LED 闪烁噪声)
//   - 阶段二：Q8 定点 IIR 低通滤波 (alpha=0.25) 平滑抖动
//   - 无浮点：全部用 Q8 定点运算 (×256 避免小数)
// ============================================================
class PpgHeartRateFilter {
public:
    static constexpr int32_t  kWindowSize  = 8;    // 滑动窗口样本数
    static constexpr int32_t  kAlphaNum    = 1;    // IIR alpha 分子: 1/4
    static constexpr int32_t  kAlphaDen    = 4;    // IIR alpha 分母

    PpgHeartRateFilter() noexcept
        : head_{0}, count_{0}, window_sum_{0}, iir_bpm_q8_{0}
    {
        for (int i = 0; i < kWindowSize; ++i) window_[i] = 0;
    }

    // C.20 Rule of Zero: 编译器自动生成拷贝/析构
    ~PpgHeartRateFilter()                          = default;
    PpgHeartRateFilter(const PpgHeartRateFilter&)  = default;
    PpgHeartRateFilter& operator=(const PpgHeartRateFilter&) = default;

    // F.6 + Con.2: 纯计算，noexcept，无副作用输出参数
    [[nodiscard]] int32_t update(uint32_t raw_bpm) noexcept {
        const int32_t raw = static_cast<int32_t>(raw_bpm);

        // --- 阶段一：滑动均值窗口 ---
        window_sum_ -= window_[head_];
        window_[head_] = raw;
        head_ = (head_ + 1) % kWindowSize;
        if (count_ < kWindowSize) ++count_;
        window_sum_ += raw;

        const int32_t windowed = (count_ > 0) ? (window_sum_ / count_) : raw;

        // --- 阶段二：Q8 IIR 低通 ---
        // y[n] = (1/4)*x + (3/4)*y[n-1]
        // 存储 iir_bpm_q8_ = BPM * 256，避免小数
        const int32_t input_q8 = windowed * 256;
        if (iir_bpm_q8_ == 0) {
            iir_bpm_q8_ = input_q8; // 首次采样直接初始化，消除启动延迟
        } else {
            // alpha=1/4: (input + 3*prev) / 4
            iir_bpm_q8_ = (kAlphaNum * input_q8
                          + (kAlphaDen - kAlphaNum) * iir_bpm_q8_)
                          / kAlphaDen;
        }

        return iir_bpm_q8_ / 256;
    }

    [[nodiscard]] int32_t get_filtered_bpm() const noexcept {
        return iir_bpm_q8_ / 256;
    }

    void reset() noexcept {
        head_ = 0; count_ = 0; window_sum_ = 0; iir_bpm_q8_ = 0;
        for (int i = 0; i < kWindowSize; ++i) window_[i] = 0;
    }

private:
    int32_t window_[kWindowSize];
    int32_t head_;
    int32_t count_;
    int64_t window_sum_;
    int32_t iir_bpm_q8_;       // BPM × 256 (Q8 定点)
};

// ============================================================
// 2. StepDetector — 动态自适应峰值检测计步器
//    输入：三轴加速度 (mg 单位, 1g ≈ 1000mg) + delta_ms
//    输出：累计步数
//
// 改进点（对比原三态机）:
//   - 动态阈值：维护最近 8 个检测到的峰值的滑动均值
//     High = peak_mean * 1.2, Low = peak_mean * 0.8
//   - 消抖窗口：两步最短间隔 250ms，防高频抖动误计
//   - 整数平方根：改用 bit-by-bit 法，全范围 int64_t 安全
// ============================================================
class StepDetector {
public:
    static constexpr int32_t  kPeakWindowSize      = 8;     // 动态阈值峰值历史数
    static constexpr uint32_t kMinStepIntervalMs   = 250;   // 步间最短时间 (ms)
    static constexpr int32_t  kDefaultHighThresh   = 1200;  // 初始高阈值 (mg)
    static constexpr int32_t  kDefaultLowThresh    = 800;   // 初始低阈值 (mg)
    static constexpr int32_t  kGravityMin          = 900;   // 1g 回稳下界 (mg)
    static constexpr int32_t  kGravityMax          = 1100;  // 1g 回稳上界 (mg)

    StepDetector() noexcept
        : total_steps_{0}
        , step_state_{State::stable}
        , time_since_step_ms_{kMinStepIntervalMs} // 允许第一步立即被计数
        , current_peak_mag_{0}
        , peak_head_{0}, peak_count_{0}, peak_sum_{0}
        , dynamic_high_{kDefaultHighThresh}
        , dynamic_low_{kDefaultLowThresh}
    {
        for (int i = 0; i < kPeakWindowSize; ++i) peak_window_[i] = 0;
    }

    ~StepDetector()                         = default;
    StepDetector(const StepDetector&)       = default;
    StepDetector& operator=(const StepDetector&) = default;

    // 每次加速度采样调用。返回最新累计步数。
    [[nodiscard]] uint32_t update(int32_t ax, int32_t ay, int32_t az,
                                  uint32_t delta_ms) noexcept
    {
        const int32_t mag = approx_magnitude(ax, ay, az);

        if (time_since_step_ms_ < kMinStepIntervalMs) {
            time_since_step_ms_ += delta_ms;
        } else {
            // 只有超过消抖窗口后才进行状态机迭代
            time_since_step_ms_ += delta_ms;
        }

        switch (step_state_) {
            case State::stable:
                if (mag > dynamic_high_) {
                    step_state_     = State::peak;
                    current_peak_mag_ = mag;
                }
                break;

            case State::peak:
                if (mag > current_peak_mag_) {
                    current_peak_mag_ = mag; // 追踪本次峰值最大值
                }
                if (mag < dynamic_low_) {
                    step_state_ = State::valley;
                }
                break;

            case State::valley:
                // 回到 1g 重力平稳区 → 完成一步
                if (mag >= kGravityMin && mag <= kGravityMax) {
                    if (time_since_step_ms_ >= kMinStepIntervalMs) {
                        ++total_steps_;
                        update_dynamic_threshold(current_peak_mag_);
                    }
                    time_since_step_ms_ = 0;
                    current_peak_mag_ = 0;
                    step_state_ = State::stable;
                }
                break;
        }

        return total_steps_;
    }

    [[nodiscard]] uint32_t get_steps() const noexcept { return total_steps_; }

    void reset() noexcept {
        total_steps_ = 0;
        step_state_  = State::stable;
        time_since_step_ms_ = kMinStepIntervalMs;
        current_peak_mag_   = 0;
        peak_head_  = 0; peak_count_ = 0; peak_sum_ = 0;
        dynamic_high_ = kDefaultHighThresh;
        dynamic_low_  = kDefaultLowThresh;
        for (int i = 0; i < kPeakWindowSize; ++i) peak_window_[i] = 0;
    }

private:
    enum class State : uint8_t { stable, peak, valley }; // Enum.3

    // 整数平方根 (bit-by-bit 法, 支持 int64_t 输入, 无 FPU 依赖)
    [[nodiscard]] static int32_t approx_magnitude(
            int32_t ax, int32_t ay, int32_t az) noexcept
    {
        const int64_t sq = static_cast<int64_t>(ax) * ax
                         + static_cast<int64_t>(ay) * ay
                         + static_cast<int64_t>(az) * az;
        if (sq <= 0) return 0;

        int64_t result    = 0;
        int64_t bit       = static_cast<int64_t>(1) << 30;
        int64_t remaining = sq;

        while (bit > remaining) bit >>= 2;

        while (bit != 0) {
            if (remaining >= result + bit) {
                remaining -= result + bit;
                result = (result >> 1) + bit;
            } else {
                result >>= 1;
            }
            bit >>= 2;
        }
        return static_cast<int32_t>(result);
    }

    void update_dynamic_threshold(int32_t peak_mag) noexcept {
        peak_sum_ -= peak_window_[peak_head_];
        peak_window_[peak_head_] = peak_mag;
        peak_head_ = (peak_head_ + 1) % kPeakWindowSize;
        if (peak_count_ < kPeakWindowSize) ++peak_count_;
        peak_sum_ += peak_mag;

        if (peak_count_ > 0) {
            const int32_t mean = static_cast<int32_t>(peak_sum_ / peak_count_);
            dynamic_high_ = (mean * 85) / 100; // 0.85× 均值
            dynamic_low_  = (mean * 60) / 100; // 0.6× 均值
        }
    }

    uint32_t total_steps_;
    State    step_state_;
    uint32_t time_since_step_ms_;
    int32_t  current_peak_mag_;
    int32_t  peak_window_[kPeakWindowSize];
    int32_t  peak_head_;
    int32_t  peak_count_;
    int64_t  peak_sum_;
    int32_t  dynamic_high_;
    int32_t  dynamic_low_;
};

// ============================================================
// 3. ActivityStateEngine — 睡眠/运动状态决策引擎
//    输入：累计步数 + 滤波后 BPM + delta_ms
//    输出：ActivityState
//
// 算法:
//   - 60 秒步频滑动窗口 (circular array of per-second step counts)
//   - 步频 (cadence_spm) = sum of 60s window  (等价于步/分钟)
//   - 静止时间计数：steps_this_second == 0 时累加
//   - 睡眠判定：静止 >= 5 分钟 AND 心率 < 65
//   - 输出 should_deep_sleep() / should_disable_wrist_wake() 供 PowerManager 使用
// ============================================================
class ActivityStateEngine {
public:
    static constexpr int32_t  kCadenceWindowSec    = 60;  // 步频统计窗口 (秒)
    static constexpr int32_t  kWalkingCadenceMin   = 60;  // 步/分 下限
    static constexpr int32_t  kRunningCadenceMin   = 130; // 步/分 下限
    static constexpr uint32_t kStillThresholdSec   = 30;  // 静止判定阈值 (秒)
    static constexpr uint32_t kSleepStillSec       = 300; // 睡眠静止时长 (秒)
    static constexpr int32_t  kSleepHRThreshold    = 65;  // 睡眠心率上限 (BPM)

    ActivityStateEngine() noexcept
        : state_{ActivityState::unknown}
        , filtered_bpm_{0}
        , still_seconds_{0}
        , cadence_head_{0}
        , cadence_sum_{0}
        , ms_accumulator_{0}
        , last_step_count_{0}
    {
        for (int i = 0; i < kCadenceWindowSec; ++i) cadence_window_[i] = 0;
    }

    ~ActivityStateEngine()                               = default;
    ActivityStateEngine(const ActivityStateEngine&)      = default;
    ActivityStateEngine& operator=(const ActivityStateEngine&) = default;

    // 每次 tick 调用。total_steps 来自 StepDetector，filtered_bpm 来自 PpgFilter。
    ActivityState update(uint32_t total_steps,
                         int32_t  filtered_bpm,
                         uint32_t delta_ms) noexcept
    {
        filtered_bpm_ = filtered_bpm;
        ms_accumulator_ += delta_ms;

        // 每秒推进一格滑动窗口
        while (ms_accumulator_ >= 1000) {
            ms_accumulator_ -= 1000;
            advance_one_second(total_steps);
        }

        resolve_state();
        return state_;
    }

    [[nodiscard]] ActivityState get_state()     const noexcept { return state_; }
    [[nodiscard]] int32_t       get_cadence()   const noexcept {
        return static_cast<int32_t>(cadence_sum_);
    }

    // PowerManager 接口
    [[nodiscard]] bool should_deep_sleep()         const noexcept {
        return state_ == ActivityState::sleeping;
    }
    // 睡眠时禁用抬腕唤醒以节省 ~0.02mA
    [[nodiscard]] bool should_disable_wrist_wake() const noexcept {
        return state_ == ActivityState::sleeping;
    }

    void reset() noexcept {
        state_ = ActivityState::unknown;
        filtered_bpm_ = 0;
        still_seconds_ = 0;
        cadence_head_ = 0; cadence_sum_ = 0;
        ms_accumulator_ = 0; last_step_count_ = 0;
        for (int i = 0; i < kCadenceWindowSec; ++i) cadence_window_[i] = 0;
    }

private:
    void advance_one_second(uint32_t total_steps) noexcept {
        const uint32_t delta_steps = total_steps - last_step_count_;
        last_step_count_ = total_steps;

        // 更新环形步频窗口
        cadence_sum_ -= cadence_window_[cadence_head_];
        const uint8_t capped = (delta_steps > 255) ? 255 : static_cast<uint8_t>(delta_steps);
        cadence_window_[cadence_head_] = capped;
        cadence_head_ = (cadence_head_ + 1) % kCadenceWindowSec;
        cadence_sum_ += capped;

        if (delta_steps == 0) {
            ++still_seconds_;
        } else {
            still_seconds_ = 0;
        }
    }

    void resolve_state() noexcept {
        const int32_t spm = static_cast<int32_t>(cadence_sum_); // steps/min

        if (spm >= kRunningCadenceMin) {
            state_ = ActivityState::running;
        } else if (spm >= kWalkingCadenceMin) {
            state_ = ActivityState::walking;
        } else if (still_seconds_ >= kSleepStillSec
                   && filtered_bpm_ > 0
                   && filtered_bpm_ < kSleepHRThreshold)
        {
            state_ = ActivityState::sleeping;
        } else if (still_seconds_ >= kStillThresholdSec) {
            // 从睡眠状态恢复需要检测到移动，不随意降级
            if (state_ != ActivityState::sleeping) {
                state_ = ActivityState::still;
            }
        } else {
            if (state_ == ActivityState::sleeping) {
                // 睡眠中检测到少量步频 → 翻身醒来，降级为 still
                if (spm > 0) state_ = ActivityState::still;
            } else {
                state_ = ActivityState::unknown;
            }
        }
    }

    ActivityState state_;
    int32_t       filtered_bpm_;
    uint32_t      still_seconds_;
    uint8_t       cadence_window_[kCadenceWindowSec];
    int32_t       cadence_head_;
    uint32_t      cadence_sum_;
    uint32_t      ms_accumulator_;
    uint32_t      last_step_count_;
};

// ============================================================
// 4. HealthAlgoEngine — 统一算法门面 (Facade)
//    聚合三个算法类，供 SensorManager 一键调用
// ============================================================
class HealthAlgoEngine {
public:
    HealthAlgoEngine() noexcept = default; // C.20 Rule of Zero

    // 喂入 PPG 原始心率，返回滤波后 BPM
    [[nodiscard]] int32_t on_ppg_sample(uint32_t raw_bpm) noexcept {
        return ppg_filter_.update(raw_bpm);
    }

    // 喂入加速度采样，返回累计步数
    [[nodiscard]] uint32_t on_accel_sample(int32_t ax, int32_t ay, int32_t az,
                                            uint32_t delta_ms) noexcept
    {
        return step_detector_.update(ax, ay, az, delta_ms);
    }

    // 更新活动状态（用上次喂入的最新数据推进时间）
    ActivityState advance_activity(uint32_t delta_ms) noexcept {
        return activity_engine_.update(
            step_detector_.get_steps(),
            ppg_filter_.get_filtered_bpm(),
            delta_ms
        );
    }

    // 便利接口
    [[nodiscard]] int32_t       get_filtered_bpm()    const noexcept { return ppg_filter_.get_filtered_bpm(); }
    [[nodiscard]] uint32_t      get_total_steps()     const noexcept { return step_detector_.get_steps(); }
    [[nodiscard]] ActivityState get_activity_state()  const noexcept { return activity_engine_.get_state(); }
    [[nodiscard]] int32_t       get_cadence_spm()     const noexcept { return activity_engine_.get_cadence(); }
    [[nodiscard]] bool          should_deep_sleep()   const noexcept { return activity_engine_.should_deep_sleep(); }
    [[nodiscard]] bool          should_disable_wrist_wake() const noexcept {
        return activity_engine_.should_disable_wrist_wake();
    }

    void reset() noexcept {
        ppg_filter_.reset();
        step_detector_.reset();
        activity_engine_.reset();
    }

private:
    PpgHeartRateFilter  ppg_filter_;
    StepDetector        step_detector_;
    ActivityStateEngine activity_engine_;
};

} // namespace health
} // namespace aurora

#endif // AURORA_HEALTH_ALGO_HPP
