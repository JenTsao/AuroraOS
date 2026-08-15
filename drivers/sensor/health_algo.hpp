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
//   - 可配置性: 每个算法模块提供 Config 聚合参数，构造时注入 (产线校准/个性化/测试)
//   - 零堆分配: 全部固定容量数组，参数运行时钳位到 [1, MAX]，不依赖 <vector>
// ============================================================

#include <stdint.h>
#include <stddef.h>

namespace aurora {
namespace health {

// ============================================================
// ActivityState — 活动状态枚举 (Enum.3: enum class)
// ============================================================
enum class ActivityState : uint8_t {
    unknown, // 初始化中，数据不足以判断
    still,   // 静止 >= 30 秒
    walking, // 步频 60~129 步/分钟
    running, // 步频 >= 130 步/分钟
    sleeping // 静止 >= 5 分钟 且 心率 < 65 BPM
};

// ============================================================
// 1. PpgHeartRateFilter — 中值预滤波 + 滑动均值 + IIR 低通滤波器
//    输入：原始 PPG 心率采样 (BPM)
//    输出：去噪后的稳定 BPM 整数值
//
// 实现：
//   - 阶段零：中值滤波预处理器 (默认窗口 5)，消除单点尖峰 (LED 闪烁/运动伪迹)
//   - 阶段一：滑动均值窗口 (默认 8) 平滑周期噪声
//   - 阶段二：Q8 定点 IIR 低通，alpha 可运行时动态调整
//     (静止/睡眠用更平滑的小 alpha，运动用更快速跟踪的大 alpha)
//   - 无浮点：全部 Q8 定点运算 (×256 避免小数)
// ============================================================
class PpgHeartRateFilter {
public:
    static constexpr int32_t kWindowSize = 8;      // 默认滑动窗口样本数
    static constexpr int32_t kMaxWindowSize = 16;  // 窗口上限 (固定数组，防溢出)
    static constexpr int32_t kMaxMedianSize = 7;   // 中值滤波窗口上限 (固定数组)
    static constexpr int32_t kAlphaNum = 1;        // 默认 IIR alpha 分子: 1/4
    static constexpr int32_t kAlphaDen = 4;        // 默认 IIR alpha 分母

    struct Config {
        int32_t window_size = kWindowSize; // 滑动均值窗口样本数
        int32_t median_size = 5;           // 中值滤波窗口 (0 = 禁用)
        int32_t alpha_num = kAlphaNum;     // IIR alpha 分子
        int32_t alpha_den = kAlphaDen;     // IIR alpha 分母
    };

    PpgHeartRateFilter() noexcept : PpgHeartRateFilter(Config{}) {}

    explicit PpgHeartRateFilter(const Config& cfg) noexcept : cfg_(cfg) {
        // 参数钳位 (固定数组，无动态分配)
        if (cfg_.window_size <= 0)
            cfg_.window_size = kWindowSize;
        if (cfg_.window_size > kMaxWindowSize)
            cfg_.window_size = kMaxWindowSize;
        if (cfg_.median_size < 0)
            cfg_.median_size = 0;
        if (cfg_.median_size > kMaxMedianSize)
            cfg_.median_size = kMaxMedianSize;
        if (cfg_.alpha_num < 0)
            cfg_.alpha_num = kAlphaNum;
        if (cfg_.alpha_den <= 0)
            cfg_.alpha_den = kAlphaDen;
        if (cfg_.alpha_num > cfg_.alpha_den)
            cfg_.alpha_num = cfg_.alpha_den;
        alpha_num_ = cfg_.alpha_num;
        alpha_den_ = cfg_.alpha_den;
        reset();
    }

    // C.20 Rule of Zero: 编译器自动生成拷贝/析构
    ~PpgHeartRateFilter() = default;
    PpgHeartRateFilter(const PpgHeartRateFilter&) = default;
    PpgHeartRateFilter& operator=(const PpgHeartRateFilter&) = default;

    // 运行时调整滤波器响应。alpha = num/den，要求 0 <= num <= den。
    void set_alpha(int32_t num, int32_t den) noexcept {
        if (num < 0)
            num = kAlphaNum;
        if (den <= 0)
            den = kAlphaDen;
        if (num > den)
            num = den;
        alpha_num_ = num;
        alpha_den_ = den;
    }

    // F.6 + Con.2: 纯计算，noexcept，无副作用输出参数
    [[nodiscard]] int32_t update(uint32_t raw_bpm) noexcept {
        const int32_t raw = static_cast<int32_t>(raw_bpm);

        // --- 阶段零：中值预滤波 (消除单点尖峰) ---
        const int32_t denoised = median_filter_(raw);

        // --- 阶段一：滑动均值窗口 ---
        window_sum_ -= window_[head_];
        window_[head_] = denoised;
        head_ = (head_ + 1) % cfg_.window_size;
        if (count_ < cfg_.window_size)
            ++count_;
        window_sum_ += denoised;

        const int32_t windowed = (count_ > 0) ? (window_sum_ / count_) : denoised;

        // --- 阶段二：Q8 IIR 低通 ---
        // y[n] = alpha*x + (1-alpha)*y[n-1]
        // 存储 iir_bpm_q8_ = BPM * 256，避免小数
        const int32_t input_q8 = windowed * 256;
        if (iir_bpm_q8_ == 0) {
            iir_bpm_q8_ = input_q8; // 首次采样直接初始化，消除启动延迟
        } else {
            iir_bpm_q8_ = (alpha_num_ * input_q8 + (alpha_den_ - alpha_num_) * iir_bpm_q8_) / alpha_den_;
        }

        return iir_bpm_q8_ / 256;
    }

    [[nodiscard]] int32_t get_filtered_bpm() const noexcept {
        return iir_bpm_q8_ / 256;
    }

    void reset() noexcept {
        head_ = 0;
        count_ = 0;
        window_sum_ = 0;
        iir_bpm_q8_ = 0;
        median_head_ = 0;
        median_count_ = 0;
        for (int i = 0; i < kMaxWindowSize; ++i)
            window_[i] = 0;
        for (int i = 0; i < kMaxMedianSize; ++i)
            median_buf_[i] = 0;
    }

private:
    // 长度为 median_size_ 的中值滤波：对最近窗口样本做插入排序取中值。
    // 插入排序 (最多 7 元素) 开销恒定，适合嵌入式实时路径。
    [[nodiscard]] int32_t median_filter_(int32_t sample) noexcept {
        if (cfg_.median_size <= 1)
            return sample;

        median_buf_[median_head_] = sample;
        median_head_ = (median_head_ + 1) % cfg_.median_size;
        if (median_count_ < cfg_.median_size)
            ++median_count_;

        int32_t tmp[kMaxMedianSize];
        for (int i = 0; i < median_count_; ++i)
            tmp[i] = median_buf_[i];
        for (int i = 1; i < median_count_; ++i) {
            const int32_t key = tmp[i];
            int j = i - 1;
            while (j >= 0 && tmp[j] > key) {
                tmp[j + 1] = tmp[j];
                --j;
            }
            tmp[j + 1] = key;
        }
        return tmp[median_count_ / 2];
    }

    int32_t window_[kMaxWindowSize];
    int32_t median_buf_[kMaxMedianSize];
    int32_t head_;
    int32_t count_;
    int64_t window_sum_;
    int32_t iir_bpm_q8_; // BPM × 256 (Q8 定点)
    int32_t median_head_;
    int32_t median_count_;
    int32_t alpha_num_; // 当前生效 alpha 分子 (可动态调整)
    int32_t alpha_den_; // 当前生效 alpha 分母
    Config cfg_;
};

// ============================================================
// 2. StepDetector — 动态自适应峰值检测计步器
//    输入：三轴加速度 (mg 单位, 1g ≈ 1000mg) + delta_ms
//    输出：累计步数
//
// 改进点（对比原三态机）:
//   - 可配置: 阈值/重力区间/消抖/校准参数全部可由 Config 注入
//   - 自适应基线校准: 静止段持续调用 calibrate()，按幅值均值±2σ
//     动态更新重力回稳区间，适配不同佩戴姿势与传感器零偏
//   - 动态阈值: 维护最近 8 个峰值的滑动均值，阈值相对基线
//     (baseline + (peak_mean - baseline)*0.85/0.6)，避免静止时误触发
//   - 抗干扰能量校验: 完成一步要求 峰谷差 >= min_peak_valley_diff，
//     排除小幅噪声在 1g 区间抖动造成的误计
//   - 谷值超时保护: valley 态停留超过 valley_timeout_ms 未回稳则丢弃该峰
//   - 消抖窗口: 两步最短间隔 250ms，防高频抖动误计
//   - 整数平方根: bit-by-bit 法，全范围 int64_t/uint64_t 安全
// ============================================================
class StepDetector {
public:
    static constexpr int32_t kPeakWindowSize = 8;       // 动态阈值峰值历史数
    static constexpr uint32_t kMinStepIntervalMs = 250; // 步间最短时间 (ms)
    static constexpr int32_t kDefaultHighThresh = 1200; // 初始高阈值 (mg)
    static constexpr int32_t kDefaultLowThresh = 800;   // 初始低阈值 (mg)
    static constexpr int32_t kGravityMin = 900;         // 默认 1g 回稳下界 (mg)
    static constexpr int32_t kGravityMax = 1100;        // 默认 1g 回稳上界 (mg)

    struct Config {
        int32_t init_high_thresh = kDefaultHighThresh; // 初始高阈值 (mg)
        int32_t init_low_thresh = kDefaultLowThresh;   // 初始低阈值 (mg)
        int32_t gravity_min = kGravityMin;             // 未校准时回稳下界 (mg)
        int32_t gravity_max = kGravityMax;             // 未校准时回稳上界 (mg)
        int32_t min_peak_valley_diff = 200;            // 完成一步的最小峰谷差 (mg, 能量校验)
        uint32_t min_step_interval_ms = kMinStepIntervalMs; // 步间最短时间 (ms)
        uint32_t calib_samples = 64;                   // 静止校准所需样本数
        uint32_t valley_timeout_ms = 2000;             // 谷值态超时 (ms)，防卡死
    };

    StepDetector() noexcept : StepDetector(Config{}) {}

    explicit StepDetector(const Config& cfg) noexcept : cfg_(cfg) {
        if (cfg_.init_high_thresh <= cfg_.init_low_thresh)
            cfg_.init_high_thresh = cfg_.init_low_thresh + 100;
        if (cfg_.min_step_interval_ms == 0)
            cfg_.min_step_interval_ms = kMinStepIntervalMs;
        if (cfg_.calib_samples == 0)
            cfg_.calib_samples = 1;
        if (cfg_.min_peak_valley_diff < 0)
            cfg_.min_peak_valley_diff = 0;
        if (cfg_.valley_timeout_ms == 0)
            cfg_.valley_timeout_ms = kMinStepIntervalMs;
        reset();
    }

    ~StepDetector() = default;
    StepDetector(const StepDetector&) = default;
    StepDetector& operator=(const StepDetector&) = default;

    // 每次加速度采样调用。返回最新累计步数。
    [[nodiscard]] uint32_t update(int32_t ax, int32_t ay, int32_t az, uint32_t delta_ms) noexcept {
        const int32_t mag = approx_magnitude(ax, ay, az);

        // 消抖窗口判断由 State::valley 分支内部完成，这里只累计距离上一步的时间
        time_since_step_ms_ += delta_ms;
        valley_timeout_ms_ += delta_ms;

        switch (step_state_) {
        case State::stable:
            if (mag > dynamic_high_) {
                step_state_ = State::peak;
                current_peak_mag_ = mag;
            }
            break;

        case State::peak:
            if (mag > current_peak_mag_) {
                current_peak_mag_ = mag; // 追踪本次峰值最大值
            }
            if (mag < dynamic_low_) {
                step_state_ = State::valley;
                current_valley_mag_ = mag;
                valley_timeout_ms_ = 0;
            }
            break;

        case State::valley:
            if (mag < current_valley_mag_)
                current_valley_mag_ = mag; // 追踪谷值最小值
            // 回到重力平稳区 → 完成一步 (要求峰谷差足够，排除噪声抖动)
            if (mag >= gravity_min() && mag <= gravity_max()) {
                if (time_since_step_ms_ >= cfg_.min_step_interval_ms &&
                    (current_peak_mag_ - current_valley_mag_) >= cfg_.min_peak_valley_diff) {
                    ++total_steps_;
                    update_dynamic_threshold(current_peak_mag_);
                }
                time_since_step_ms_ = 0;
                current_peak_mag_ = 0;
                current_valley_mag_ = 0;
                step_state_ = State::stable;
            } else if (valley_timeout_ms_ >= cfg_.valley_timeout_ms) {
                // 谷值超时未回稳 → 丢弃本次峰 (可能是噪声)，回到稳定态
                time_since_step_ms_ = 0;
                current_peak_mag_ = 0;
                current_valley_mag_ = 0;
                step_state_ = State::stable;
            }
            break;
        }

        return total_steps_;
    }

    // 静止段自适应基线校准：累计幅值样本，达到 calib_samples 后
    // 以 mean ± 2σ 更新重力回稳区间，并刷新阈值基线。
    // 调用方 (SensorManager) 仅在检测到连续静止时调用。
    void calibrate(int32_t ax, int32_t ay, int32_t az) noexcept {
        const int32_t mag = approx_magnitude(ax, ay, az);
        calib_sum_ += static_cast<uint32_t>(mag);
        calib_sum_sq_ += static_cast<uint64_t>(mag) * static_cast<uint64_t>(mag);
        ++calib_count_;

        if (calib_count_ >= cfg_.calib_samples)
            finalize_calibration();
    }

    [[nodiscard]] uint32_t get_steps() const noexcept {
        return total_steps_;
    }

    void reset() noexcept {
        total_steps_ = 0;
        step_state_ = State::stable;
        time_since_step_ms_ = kMinStepIntervalMs;
        valley_timeout_ms_ = 0;
        current_peak_mag_ = 0;
        current_valley_mag_ = 0;
        peak_head_ = 0;
        peak_count_ = 0;
        peak_sum_ = 0;
        dynamic_high_ = cfg_.init_high_thresh;
        dynamic_low_ = cfg_.init_low_thresh;
        baseline_mg_ = 1000; // 默认重力基线
        calibrated_gravity_min_ = -1; // -1 = 未校准
        calibrated_gravity_max_ = -1;
        calib_sum_ = 0;
        calib_sum_sq_ = 0;
        calib_count_ = 0;
        for (int i = 0; i < kPeakWindowSize; ++i)
            peak_window_[i] = 0;
    }

private:
    enum class State : uint8_t {
        stable,
        peak,
        valley
    }; // Enum.3

    // 整数平方根 (bit-by-bit 法, 支持 uint64_t 输入, 无 FPU 依赖)
    [[nodiscard]] static int32_t integer_sqrt(uint64_t v) noexcept {
        if (v == 0)
            return 0;

        uint64_t result = 0;
        uint64_t bit = static_cast<uint64_t>(1) << 62;

        while (bit > v)
            bit >>= 2;

        while (bit != 0) {
            if (v >= result + bit) {
                v -= result + bit;
                result = (result >> 1) + bit;
            } else {
                result >>= 1;
            }
            bit >>= 2;
        }
        return static_cast<int32_t>(result);
    }

    [[nodiscard]] static int32_t approx_magnitude(int32_t ax, int32_t ay, int32_t az) noexcept {
        const uint64_t sq = static_cast<uint64_t>(ax) * static_cast<uint64_t>(ax) +
                            static_cast<uint64_t>(ay) * static_cast<uint64_t>(ay) +
                            static_cast<uint64_t>(az) * static_cast<uint64_t>(az);
        return integer_sqrt(sq);
    }

    // 生效中的重力回稳下界 (校准后优先)
    [[nodiscard]] int32_t gravity_min() const noexcept {
        return (calibrated_gravity_min_ >= 0) ? calibrated_gravity_min_ : cfg_.gravity_min;
    }

    [[nodiscard]] int32_t gravity_max() const noexcept {
        return (calibrated_gravity_max_ >= 0) ? calibrated_gravity_max_ : cfg_.gravity_max;
    }

    void update_dynamic_threshold(int32_t peak_mag) noexcept {
        peak_sum_ -= peak_window_[peak_head_];
        peak_window_[peak_head_] = peak_mag;
        peak_head_ = (peak_head_ + 1) % kPeakWindowSize;
        if (peak_count_ < kPeakWindowSize)
            ++peak_count_;
        peak_sum_ += peak_mag;

        if (peak_count_ > 0) {
            const int32_t mean = static_cast<int32_t>(peak_sum_ / peak_count_);
            // 阈值相对重力基线: baseline + (peak_mean - baseline) * factor
            // 峰值低于基线 (异常) 时回退到初始阈值，避免静止误触发
            if (mean > baseline_mg_) {
                const int32_t offset = mean - baseline_mg_;
                dynamic_high_ = baseline_mg_ + (offset * 85) / 100; // 0.85× 幅值
                dynamic_low_ = baseline_mg_ + (offset * 60) / 100;  // 0.6× 幅值
            } else {
                dynamic_high_ = cfg_.init_high_thresh;
                dynamic_low_ = cfg_.init_low_thresh;
            }
        }
    }

    void finalize_calibration() noexcept {
        const uint32_t n = calib_count_;
        if (n == 0)
            return;

        // mean 与方差 (E[x^2] - E[x]^2)，全部整数运算
        const int64_t mean = static_cast<int64_t>(calib_sum_) / n;
        const int64_t mean_sq = static_cast<int64_t>(calib_sum_sq_) / n;
        int64_t var = mean_sq - mean * mean;
        if (var < 0)
            var = 0;
        const int32_t std = integer_sqrt(static_cast<uint64_t>(var));

        baseline_mg_ = static_cast<int32_t>(mean);
        calibrated_gravity_min_ = baseline_mg_ - 2 * std;
        calibrated_gravity_max_ = baseline_mg_ + 2 * std;

        // 区间过窄 (样本可能仍含运动) 时退回默认范围
        if (calibrated_gravity_max_ - calibrated_gravity_min_ < 50) {
            calibrated_gravity_min_ = cfg_.gravity_min;
            calibrated_gravity_max_ = cfg_.gravity_max;
        }

        // 校准后基于新基线重设阈值
        dynamic_high_ = cfg_.init_high_thresh;
        dynamic_low_ = cfg_.init_low_thresh;

        calib_sum_ = 0;
        calib_sum_sq_ = 0;
        calib_count_ = 0;
    }

    uint32_t total_steps_;
    State step_state_;
    uint32_t time_since_step_ms_;
    uint32_t valley_timeout_ms_;
    int32_t current_peak_mag_;
    int32_t current_valley_mag_;
    int32_t peak_window_[kPeakWindowSize];
    int32_t peak_head_;
    int32_t peak_count_;
    int64_t peak_sum_;
    int32_t dynamic_high_;
    int32_t dynamic_low_;
    int32_t baseline_mg_;          // 重力基线 (默认 1000)
    int32_t calibrated_gravity_min_; // -1 = 未校准
    int32_t calibrated_gravity_max_;
    uint32_t calib_sum_;           // 校准幅值和
    uint64_t calib_sum_sq_;        // 校准幅值平方和
    uint32_t calib_count_;         // 已累计校准样本
    Config cfg_;
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
//   - 睡眠进入：静止 >= 5 分钟 AND 心率 < 65，且连续满足
//     sleep_enter_confirm_sec 秒 (置信度计数器，防白天静坐误判)
//   - 睡眠退出：持续运动 (spm >= min_exit_cadence 且连续
//     sleep_motion_confirm_sec 秒) 或心率持续上升，累计
//     sleep_exit_confirm_sec 秒才退出 (缓冲退出，防翻身误醒)
//   - 输出 should_deep_sleep() / should_disable_wrist_wake() 供 PowerManager 使用
// ============================================================
class ActivityStateEngine {
public:
    static constexpr int32_t kCadenceWindowSec = 60;    // 步频统计窗口 (秒)
    static constexpr int32_t kWalkingCadenceMin = 60;   // 步/分 下限
    static constexpr int32_t kRunningCadenceMin = 130;  // 步/分 下限
    static constexpr uint32_t kStillThresholdSec = 30;  // 静止判定阈值 (秒)
    static constexpr uint32_t kSleepStillSec = 300;     // 睡眠静止时长 (秒)
    static constexpr int32_t kSleepHRThreshold = 65;    // 睡眠心率上限 (BPM)
    static constexpr uint32_t kSleepEnterConfirmSec = 60; // 进入睡眠连续确认 (秒)
    static constexpr uint32_t kSleepExitConfirmSec = 30;  // 退出睡眠缓冲 (秒)
    static constexpr uint32_t kSleepMotionConfirmSec = 10; // 睡眠中连续运动确认 (秒)
    static constexpr int32_t kMinExitCadence = 10;       // 睡眠中视作运动的最小步频 (spm)
    static constexpr int32_t kHRRiseExitBpm = 15;        // 心率相对基线上升阈值 (BPM)
    static constexpr uint32_t kHRRiseConfirmSec = 3;     // 心率持续上升确认 (秒)

    struct Config {
        uint32_t cadence_window_sec = kCadenceWindowSec; // 步频窗口 (≤ 60)
        int32_t walking_cadence_min = kWalkingCadenceMin;
        int32_t running_cadence_min = kRunningCadenceMin;
        uint32_t still_threshold_sec = kStillThresholdSec;
        uint32_t sleep_still_sec = kSleepStillSec;
        int32_t sleep_hr_threshold = kSleepHRThreshold;
        uint32_t sleep_enter_confirm_sec = kSleepEnterConfirmSec;
        uint32_t sleep_exit_confirm_sec = kSleepExitConfirmSec;
        uint32_t sleep_motion_confirm_sec = kSleepMotionConfirmSec;
        int32_t min_exit_cadence = kMinExitCadence;
        int32_t hr_rise_exit_bpm = kHRRiseExitBpm;
    };

    ActivityStateEngine() noexcept : ActivityStateEngine(Config{}) {}

    explicit ActivityStateEngine(const Config& cfg) noexcept
        : cfg_(cfg), state_{ActivityState::unknown}, filtered_bpm_{0}, still_seconds_{0}, cadence_head_{0},
          cadence_sum_{0}, ms_accumulator_{0}, last_step_count_{0}, sleep_enter_counter_{0},
          sleep_exit_counter_{0}, sleep_motion_seconds_{0}, hr_baseline_bpm_{0}, hr_rising_seconds_{0} {
        if (cfg_.cadence_window_sec == 0 || cfg_.cadence_window_sec > kCadenceWindowSec)
            cfg_.cadence_window_sec = kCadenceWindowSec;
        if (cfg_.walking_cadence_min <= 0)
            cfg_.walking_cadence_min = kWalkingCadenceMin;
        if (cfg_.running_cadence_min <= cfg_.walking_cadence_min)
            cfg_.running_cadence_min = cfg_.walking_cadence_min + 1;
        if (cfg_.sleep_enter_confirm_sec == 0)
            cfg_.sleep_enter_confirm_sec = kSleepEnterConfirmSec;
        if (cfg_.sleep_exit_confirm_sec == 0)
            cfg_.sleep_exit_confirm_sec = kSleepExitConfirmSec;
        if (cfg_.sleep_motion_confirm_sec == 0)
            cfg_.sleep_motion_confirm_sec = kSleepMotionConfirmSec;
        if (cfg_.min_exit_cadence <= 0)
            cfg_.min_exit_cadence = kMinExitCadence;
        if (cfg_.hr_rise_exit_bpm <= 0)
            cfg_.hr_rise_exit_bpm = kHRRiseExitBpm;
        for (int i = 0; i < kCadenceWindowSec; ++i)
            cadence_window_[i] = 0;
    }

    ~ActivityStateEngine() = default;
    ActivityStateEngine(const ActivityStateEngine&) = default;
    ActivityStateEngine& operator=(const ActivityStateEngine&) = default;

    // 每次 tick 调用。total_steps 来自 StepDetector，filtered_bpm 来自 PpgFilter。
    ActivityState update(uint32_t total_steps, int32_t filtered_bpm, uint32_t delta_ms) noexcept {
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

    [[nodiscard]] ActivityState get_state() const noexcept {
        return state_;
    }

    [[nodiscard]] int32_t get_cadence() const noexcept {
        return static_cast<int32_t>(cadence_sum_);
    }

    // PowerManager 接口
    [[nodiscard]] bool should_deep_sleep() const noexcept {
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
        cadence_head_ = 0;
        cadence_sum_ = 0;
        ms_accumulator_ = 0;
        last_step_count_ = 0;
        sleep_enter_counter_ = 0;
        sleep_exit_counter_ = 0;
        sleep_motion_seconds_ = 0;
        hr_baseline_bpm_ = 0;
        hr_rising_seconds_ = 0;
        for (int i = 0; i < kCadenceWindowSec; ++i)
            cadence_window_[i] = 0;
    }

private:
    void advance_one_second(uint32_t total_steps) noexcept {
        const uint32_t delta_steps = total_steps - last_step_count_;
        last_step_count_ = total_steps;

        // 更新环形步频窗口
        const int32_t len = static_cast<int32_t>(cfg_.cadence_window_sec);
        cadence_sum_ -= cadence_window_[cadence_head_];
        const uint8_t capped = (delta_steps > 255) ? 255 : static_cast<uint8_t>(delta_steps);
        cadence_window_[cadence_head_] = capped;
        cadence_head_ = (cadence_head_ + 1) % len;
        cadence_sum_ += capped;

        if (delta_steps == 0) {
            ++still_seconds_;
        } else {
            still_seconds_ = 0;
        }
    }

    // 心率基线跟踪：心率持续上升超过阈值时累计 hr_rising_seconds_，
    // 回落时重置基线并清零。睡眠期可作为"生理唤醒"辅助信号。
    void track_heart_rate(int32_t bpm) noexcept {
        if (bpm <= 0) {
            hr_rising_seconds_ = 0;
            return;
        }
        if (hr_baseline_bpm_ == 0) {
            hr_baseline_bpm_ = bpm;
            hr_rising_seconds_ = 0;
            return;
        }
        if (bpm >= hr_baseline_bpm_ + cfg_.hr_rise_exit_bpm) {
            ++hr_rising_seconds_;
        } else {
            hr_rising_seconds_ = 0;
            if (bpm < hr_baseline_bpm_)
                hr_baseline_bpm_ = bpm; // 基线缓慢跟降
        }
    }

    void resolve_state() noexcept {
        track_heart_rate(filtered_bpm_);

        const int32_t spm = static_cast<int32_t>(cadence_sum_); // steps/min
        const bool sleep_ok = (still_seconds_ >= cfg_.sleep_still_sec) && (filtered_bpm_ > 0) &&
                              (filtered_bpm_ < cfg_.sleep_hr_threshold);
        const bool hr_rising = (hr_rising_seconds_ >= kHRRiseConfirmSec);

        if (spm >= cfg_.running_cadence_min) {
            state_ = ActivityState::running;
            sleep_enter_counter_ = 0;
            sleep_exit_counter_ = 0;
            sleep_motion_seconds_ = 0;
        } else if (spm >= cfg_.walking_cadence_min) {
            state_ = ActivityState::walking;
            sleep_enter_counter_ = 0;
            sleep_exit_counter_ = 0;
            sleep_motion_seconds_ = 0;
        } else if (sleep_ok) {
            // 进入睡眠需连续满足确认窗口 (置信度计数器)
            if (sleep_enter_counter_ < cfg_.sleep_enter_confirm_sec) {
                ++sleep_enter_counter_;
            } else if (state_ != ActivityState::sleeping) {
                state_ = ActivityState::sleeping;
                sleep_exit_counter_ = 0;
                sleep_motion_seconds_ = 0;
                hr_baseline_bpm_ = filtered_bpm_; // 以睡眠心率为新基线
            }
        } else if (state_ == ActivityState::sleeping) {
            // 睡眠中：持续运动或心率持续上升才累积退出计数 (缓冲退出)
            const bool motion_now = (spm >= cfg_.min_exit_cadence);
            if (motion_now) {
                ++sleep_motion_seconds_;
                if (sleep_motion_seconds_ >= cfg_.sleep_motion_confirm_sec)
                    ++sleep_exit_counter_;
            } else {
                sleep_motion_seconds_ = 0;
            }
            if (hr_rising)
                ++sleep_exit_counter_;

            if (sleep_exit_counter_ >= cfg_.sleep_exit_confirm_sec) {
                state_ = ActivityState::still;
                sleep_enter_counter_ = 0;
                sleep_exit_counter_ = 0;
                sleep_motion_seconds_ = 0;
            }
        } else if (still_seconds_ >= cfg_.still_threshold_sec) {
            if (state_ != ActivityState::sleeping) {
                state_ = ActivityState::still;
                sleep_enter_counter_ = 0;
            }
        } else {
            if (state_ != ActivityState::sleeping) {
                // 有活动但步频未达 walking 阈值 (如睡眠唤醒后的低步频活动)：
                // 保持 still，不降级为 unknown，避免睡眠退出后状态抖动
                if (state_ == ActivityState::still && spm > 0) {
                    state_ = ActivityState::still;
                } else {
                    state_ = ActivityState::unknown;
                }
                sleep_enter_counter_ = 0;
            }
        }
    }

    Config cfg_;
    ActivityState state_;
    int32_t filtered_bpm_;
    uint32_t still_seconds_;
    uint8_t cadence_window_[kCadenceWindowSec];
    int32_t cadence_head_;
    uint32_t cadence_sum_;
    uint32_t ms_accumulator_;
    uint32_t last_step_count_;
    uint32_t sleep_enter_counter_;   // 连续满足睡眠条件的秒数
    uint32_t sleep_exit_counter_;    // 睡眠中连续运动/心率上升的秒数
    uint32_t sleep_motion_seconds_;  // 睡眠中步频达标的连续秒数
    int32_t hr_baseline_bpm_;        // 心率参考基线
    uint32_t hr_rising_seconds_;     // 心率持续上升秒数
};

// ============================================================
// 4. HealthAlgoEngine — 统一算法门面 (Facade)
//    聚合三个算法类，供 SensorManager 一键调用
//    支持整体 Config 注入 (PPG/计步/活动状态分别可调)
//    根据活动状态自动切换心率滤波 alpha (静止平滑 / 运动快速)
// ============================================================
class HealthAlgoEngine {
public:
    static constexpr int32_t kAlphaDen = 256;
    static constexpr int32_t kAlphaSlowNum = 38; // ≈0.148，静止/睡眠更平滑
    static constexpr int32_t kAlphaFastNum = 90; // ≈0.352，运动更快跟踪

    struct Config {
        PpgHeartRateFilter::Config ppg;
        StepDetector::Config step;
        ActivityStateEngine::Config activity;
    };

    HealthAlgoEngine() noexcept : HealthAlgoEngine(Config{}) {}

    explicit HealthAlgoEngine(const Config& cfg) noexcept
        : ppg_filter_(cfg.ppg), step_detector_(cfg.step), activity_engine_(cfg.activity) {}

    // 喂入 PPG 原始心率，返回滤波后 BPM
    [[nodiscard]] int32_t on_ppg_sample(uint32_t raw_bpm) noexcept {
        return ppg_filter_.update(raw_bpm);
    }

    // 喂入加速度采样，返回累计步数
    [[nodiscard]] uint32_t on_accel_sample(int32_t ax, int32_t ay, int32_t az, uint32_t delta_ms) noexcept {
        return step_detector_.update(ax, ay, az, delta_ms);
    }

    // 更新活动状态（用上次喂入的最新数据推进时间），并按新状态调整心率滤波响应
    ActivityState advance_activity(uint32_t delta_ms) noexcept {
        const ActivityState st = activity_engine_.update(step_detector_.get_steps(), ppg_filter_.get_filtered_bpm(),
                                                         delta_ms);
        apply_adaptive_alpha(st);
        return st;
    }

    // 静止段加速度基线校准 (由 SensorManager 在检测到连续静止时调用)
    void calibrate_accelerometer(int32_t ax, int32_t ay, int32_t az) noexcept {
        step_detector_.calibrate(ax, ay, az);
    }

    // 运行期手动调整心率滤波响应 (产线校准/调试接口)
    void set_hr_filter_alpha(int32_t num, int32_t den) noexcept {
        ppg_filter_.set_alpha(num, den);
    }

    // 便利接口
    [[nodiscard]] int32_t get_filtered_bpm() const noexcept {
        return ppg_filter_.get_filtered_bpm();
    }

    [[nodiscard]] uint32_t get_total_steps() const noexcept {
        return step_detector_.get_steps();
    }

    [[nodiscard]] ActivityState get_activity_state() const noexcept {
        return activity_engine_.get_state();
    }

    [[nodiscard]] int32_t get_cadence_spm() const noexcept {
        return activity_engine_.get_cadence();
    }

    [[nodiscard]] bool should_deep_sleep() const noexcept {
        return activity_engine_.should_deep_sleep();
    }

    [[nodiscard]] bool should_disable_wrist_wake() const noexcept {
        return activity_engine_.should_disable_wrist_wake();
    }

    void reset() noexcept {
        ppg_filter_.reset();
        step_detector_.reset();
        activity_engine_.reset();
    }

private:
    // 根据活动状态动态调整心率滤波 alpha：
    //   walking/running → 快速跟踪 (更快响应心率突变)
    //   still/sleeping/unknown → 平滑滤波 (抑制噪声)
    void apply_adaptive_alpha(ActivityState st) noexcept {
        const bool active = (st == ActivityState::walking) || (st == ActivityState::running);
        if (active)
            ppg_filter_.set_alpha(kAlphaFastNum, kAlphaDen);
        else
            ppg_filter_.set_alpha(kAlphaSlowNum, kAlphaDen);
    }

    PpgHeartRateFilter ppg_filter_;
    StepDetector step_detector_;
    ActivityStateEngine activity_engine_;
};

} // namespace health
} // namespace aurora

#endif // AURORA_HEALTH_ALGO_HPP
