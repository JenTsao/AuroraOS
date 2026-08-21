// =============================================================================
// drivers/rf/spectrum_monitor.hpp
//
// 射频频谱感知守护引擎（集成层）
//
// 组合 ISpectrumSensor + RfAnalyzer + JammingDetector，提供：
//   - SpectrumMonitor 单例：单帧推进异常检测与干扰识别，维护告警环形缓冲，
//     按严重度/冷却窗口联动 SecurityMonitor 上报告警
//   - RfSpectrumNode：/proc/rf_spectrum 只读状态节点（扫频次数/校准态/最近告警）
//   - NullSpectrumSensor：默认被动传感器，平坦噪声底，无真实射频前端时安全运行
//
// 设计原则（遵循 AGENTS.md）：
//   - 组合优于继承：持有 RfAnalyzer + JammingDetector 引用/成员，单一职责
//   - 固定环形缓冲，零动态内存分配，全定点、noexcept
//   - 纯计算无锁；仅告警环形缓冲以 Mutex 保护（守护任务写入，ProcFS 读取）
//   - 模块通过 SecurityMonitor 上报，但不依赖具体硬件，也不引入反向依赖
// =============================================================================
#ifndef AURORA_RF_SPECTRUM_MONITOR_HPP
#define AURORA_RF_SPECTRUM_MONITOR_HPP

#include <stdint.h>
#include "spectrum_sensor.hpp"
#include "rf_analyzer.hpp"
#include "jamming_detector.hpp"
#include "../../kernel/core/mutex.hpp"
#include "../../kernel/core/security_monitor.hpp"
#include "../../vfs/vfs.hpp"
#include "../../vfs/procfs.hpp"

namespace aurora {
namespace rf {

// ---------------------------------------------------------------------------
// 告警环形缓冲容量与单帧异常批量上限
// ---------------------------------------------------------------------------
constexpr int kMaxSpectrumAlerts = 64; // 最近告警环形缓冲
constexpr int kMaxAnomalyBatch = 8;    // 单次扫频输出的最大分箱级异常数

// 告警来源类型
enum class SpectrumAlertKind : uint8_t {
    Anomaly = 0, // 分箱级异常（RfAnalyzer）
    Jamming = 1, // 跨帧干扰（JammingDetector）
};

// 统一频谱告警条目
struct SpectrumAlert {
    uint32_t timestamp_ms; // 时间戳
    uint16_t freq_mhz;     // 中心频率（异常时触发分箱频率）
    uint16_t bandwidth_khz; // 干扰带宽估算（异常时为 0）
    uint8_t severity;       // 严重度 0-100
    uint8_t confidence;     // 置信度 0-100（异常时为 0）
    SpectrumAlertKind kind; // 来源类型
    uint8_t type;           // AnomalyType / JammingType 数值
    char description[64];   // 可读描述
};

// ---------------------------------------------------------------------------
// ProcFS 节点: /proc/rf_spectrum
// ---------------------------------------------------------------------------
class RfSpectrumNode : public ProcNode {
public:
    void set_monitor(class SpectrumMonitor* m) {
        monitor_ = m;
    }

    int read(char* buf, int len, int offset, void* priv) override;

private:
    class SpectrumMonitor* monitor_ = nullptr;
};

// ---------------------------------------------------------------------------
// NullSpectrumSensor：默认被动传感器（无真实射频前端时的安全占位）
//
// 返回一段平坦噪声底（-90 dBm），使分析器能正常建立基线而不产生误报。
// 接入真实 SDR/频谱芯片时，实现 ISpectrumSensor 替换即可，上层无需改动。
// ---------------------------------------------------------------------------
class NullSpectrumSensor : public ISpectrumSensor {
public:
    NullSpectrumSensor() {
        configure(2400, 1000, 16);
    }

    void configure(uint16_t start_mhz, uint16_t step_khz, uint8_t count) noexcept {
        start_freq_ = start_mhz;
        step_khz_ = step_khz;
        bin_count_ = (count > kMaxSpectrumBins) ? kMaxSpectrumBins : count;
        for (uint8_t i = 0; i < bin_count_; ++i) {
            freq_[i] = static_cast<uint16_t>(start_mhz + (i * step_khz_ / 1000u));
        }
    }

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
            out->bins[i].freq_mhz = freq_[i];
            out->bins[i].power_q8 = power::from_dbm(kNoiseFloorDbm);
        }
        now_ms_ += kSweepPeriodMs;
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
    static constexpr int16_t kNoiseFloorDbm = -90; // 默认平坦噪声底
    static constexpr uint32_t kSweepPeriodMs = 100; // 模拟扫频周期

    uint16_t freq_[kMaxSpectrumBins]{};
    uint8_t bin_count_ = 0;
    uint16_t start_freq_ = 2400;
    uint16_t step_khz_ = 1000;
    uint32_t now_ms_ = 0;
    bool powered_ = false;
};

// ---------------------------------------------------------------------------
// 频谱守护任务（定义于 spectrum_monitor.cpp）
// ---------------------------------------------------------------------------
// 后台任务入口：周期扫频并单线程推进 SpectrumMonitor。
void spectrum_monitor_task_entry();

// 创建低优先级频谱守护任务，成功返回 true。
bool create_spectrum_monitor_task();

// ---------------------------------------------------------------------------
// SpectrumMonitor：频谱感知守护引擎
// ---------------------------------------------------------------------------
class SpectrumMonitor {
public:
    static SpectrumMonitor& instance() {
        static SpectrumMonitor mon;
        return mon;
    }

    // 绑定传感器并挂载 /proc/rf_spectrum（幂等）。
    // sensor 为 nullptr 时使用默认 NullSpectrumSensor；重复调用可替换传感器。
    void init(ISpectrumSensor* sensor = nullptr) {
        if (sensor) {
            sensor_ = sensor;
        } else if (!sensor_) {
            static NullSpectrumSensor default_sensor;
            sensor_ = &default_sensor;
        }

        if (initialized_)
            return;
        node_.set_monitor(this);
        VfsManager::instance().mount("/proc/rf_spectrum", &node_);
        initialized_ = true;
    }

    ISpectrumSensor* get_sensor() const noexcept {
        return sensor_;
    }

    // 处理一次扫频：先推进异常检测（并正确捕获 SuddenBurst），再复用刚更新的
    // 基线做干扰识别（不重复推进噪声底）。记录告警并按需联动 SecurityMonitor。
    void process_sweep(const SpectrumSweep& sweep) {
        ++sweep_count_;
        last_timestamp_ms_ = sweep.timestamp_ms;

        // 1. 分箱级异常检测（推进基线）
        AnomalyEvent anomalies[kMaxAnomalyBatch];
        const int na = analyzer_.analyze(sweep, anomalies, kMaxAnomalyBatch);
        for (int i = 0; i < na; ++i) {
            record_anomaly_(anomalies[i]);
        }

        // 2. 跨帧干扰识别（复用刚更新的基线，避免同帧重复推进）
        const JammingAlert* jam = jamming_.detect(sweep, /*advance_baseline=*/false);
        if (jam) {
            record_jamming_(*jam);
        }
    }

    // ---- 查询 ----

    uint32_t get_sweep_count() const noexcept {
        return sweep_count_;
    }

    uint32_t get_alert_count() const noexcept {
        return alert_count_;
    }

    bool is_calibrated() const noexcept {
        return analyzer_.is_calibrated();
    }

    // 返回告警序号 index 对应的条目（环形缓冲，index 为写入序号）
    const SpectrumAlert* get_alert(int index) const {
        if (index < 0)
            return nullptr;
        return &alerts_[static_cast<uint32_t>(index) % kMaxSpectrumAlerts];
    }

    PowerQ8 get_noise_floor(uint8_t bin_index) const noexcept {
        return analyzer_.get_noise_floor(bin_index);
    }

    // 重置分析器/干扰检测器状态与告警缓冲（噪声底重新建基线）
    void reset() noexcept {
        analyzer_.reset();
        jamming_.reset();
        alert_count_ = 0;
        sweep_count_ = 0;
        last_timestamp_ms_ = 0;
        last_report_ms_ = 0;
    }

    // ---- 配置透传 ----

    void set_noise_margin_dbm(int16_t dbm) noexcept {
        analyzer_.set_noise_margin_dbm(dbm);
    }

    void set_absolute_max_dbm(int16_t dbm) noexcept {
        analyzer_.set_absolute_max_dbm(dbm);
    }

    void set_burst_delta_dbm(int16_t dbm) noexcept {
        analyzer_.set_burst_delta_dbm(dbm);
    }

    void set_wideband_ratio(uint8_t percent) noexcept {
        analyzer_.set_wideband_ratio(percent);
        jamming_.set_wideband_ratio(percent);
    }

    // SecurityMonitor 联动冷却窗口（ms），0 表示每次告警都上报
    void set_report_cooldown_ms(uint32_t ms) noexcept {
        report_cooldown_ms_ = ms;
    }

private:
    SpectrumMonitor() : jamming_(analyzer_) {}

    ISpectrumSensor* sensor_ = nullptr;
    RfAnalyzer analyzer_;
    JammingDetector jamming_; // 依赖 analyzer_，须在其后声明

    RfSpectrumNode node_;

    SpectrumAlert alerts_[kMaxSpectrumAlerts]{};
    volatile uint32_t alert_count_ = 0; // 写入序号（单调递增）
    Mutex alert_mutex_;

    uint32_t sweep_count_ = 0;
    uint32_t last_timestamp_ms_ = 0;
    uint32_t last_report_ms_ = 0;
    uint32_t report_cooldown_ms_ = 1000;

    bool initialized_ = false;

    // 高严重度异常（>= 该阈值）才联动 SecurityMonitor，抑制噪声级误报
    static constexpr uint8_t kReportSeverityThreshold = 70;

    static uint32_t now_ms_() noexcept {
        extern volatile uint32_t tick_count;
        return tick_count;
    }

    void push_alert_(const SpectrumAlert& a) {
        LockGuard lock(alert_mutex_);
        alerts_[alert_count_ % kMaxSpectrumAlerts] = a;
        ++alert_count_;
    }

    // 基于冷却窗口决定是否上报告警（首条始终上报）
    bool should_report_(uint32_t ts) const noexcept {
        if (report_cooldown_ms_ == 0)
            return true;
        return (ts - last_report_ms_) >= report_cooldown_ms_;
    }

    void report_(const char* desc) {
        const uint32_t ts = now_ms_();
        if (!should_report_(ts))
            return;
        last_report_ms_ = ts;
        SecurityMonitor::instance().report_firewall_anomaly(desc);
    }

    void record_anomaly_(const AnomalyEvent& ev) {
        SpectrumAlert a{};
        a.timestamp_ms = ev.timestamp_ms;
        a.freq_mhz = ev.freq_mhz;
        a.bandwidth_khz = 0;
        a.severity = ev.severity;
        a.confidence = 0;
        a.kind = SpectrumAlertKind::Anomaly;
        a.type = static_cast<uint8_t>(ev.type);
        fill_desc_(a, RfAnalyzer::type_name(ev.type), ev.freq_mhz);
        push_alert_(a);

        if (ev.severity >= kReportSeverityThreshold) {
            report_(a.description);
        }
    }

    void record_jamming_(const JammingAlert& ja) {
        SpectrumAlert a{};
        a.timestamp_ms = ja.timestamp_ms;
        a.freq_mhz = ja.center_freq_mhz;
        a.bandwidth_khz = ja.bandwidth_khz;
        a.severity = ja.severity;
        a.confidence = ja.confidence;
        a.kind = SpectrumAlertKind::Jamming;
        a.type = static_cast<uint8_t>(ja.type);
        // 拷贝描述（固定缓冲，手动拷贝避免依赖字符串库）
        for (int i = 0; i < static_cast<int>(sizeof(a.description)) - 1; ++i) {
            a.description[i] = ja.description[i];
            if (ja.description[i] == '\0')
                break;
        }
        a.description[sizeof(a.description) - 1] = '\0';
        push_alert_(a);

        // 干扰识别结果直接联动 SecurityMonitor
        report_(a.description);
    }

    // 填充可读描述（零堆分配，手动十进制拼接）
    static void fill_desc_(SpectrumAlert& a, const char* type_name, uint16_t freq_mhz) noexcept {
        char* p = a.description;
        const char* const end = a.description + sizeof(a.description) - 1;

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

        app_s("RF anomaly: ");
        app_s(type_name);
        app_s(" @ ");
        app_u(freq_mhz);
        app_s(" MHz");
        *p = '\0';
    }

    friend class RfSpectrumNode;
};

// =============================================================================
// ProcFS 节点实现（header-only，与 wireless_ids.hpp 一致）
// =============================================================================
inline int RfSpectrumNode::read(char* buf, int len, int /*offset*/, void* /*priv*/) {
    if (!monitor_)
        return 0;

    int pos = 0;
    auto app_s = [&](const char* s) {
        while (*s && pos < len - 1)
            buf[pos++] = *s++;
    };
    auto app_u = [&](uint32_t v) {
        char tmp[16];
        int i = 0;
        if (v == 0) {
            tmp[i++] = '0';
        }
        while (v > 0) {
            tmp[i++] = static_cast<char>('0' + (v % 10u));
            v /= 10u;
        }
        while (i > 0 && pos < len - 1)
            buf[pos++] = tmp[--i];
    };

    app_s("RF Spectrum Sensing Status\n");
    app_s("===========================\n");
    app_s("Sweeps: ");
    app_u(monitor_->get_sweep_count());
    app_s("\n");
    app_s("Calibrated: ");
    app_s(monitor_->is_calibrated() ? "yes" : "no");
    app_s("\n");
    app_s("Alerts: ");
    app_u(monitor_->get_alert_count());
    app_s("\n");
    app_s("\n--- Recent Alerts ---\n");

    const uint32_t total = monitor_->get_alert_count();
    const uint32_t start = (total > 20) ? (total - 20) : 0;
    for (uint32_t i = start; i < total && pos < len - 80; ++i) {
        const SpectrumAlert* a = monitor_->get_alert(static_cast<int>(i));
        if (!a)
            continue;

        app_u(a->timestamp_ms);
        app_s(" ");
        app_s(a->kind == SpectrumAlertKind::Jamming ? "jam " : "anom");
        app_s(" [");
        app_u(a->severity);
        app_s("] ");
        app_s(a->description);
        app_s("\n");
    }

    buf[pos] = '\0';
    return pos;
}

} // namespace rf
} // namespace aurora

#endif // AURORA_RF_SPECTRUM_MONITOR_HPP
