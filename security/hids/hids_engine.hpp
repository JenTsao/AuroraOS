// =============================================================================
// security/hids/hids_engine.hpp
//
// HIDS 核心引擎：组合文件完整性 + 进程监控 + 权限审计 + Rootkit 扫描
//
//   - HidsEngine 单例：tick() 驱动 4 个检测模块，将发现转为告警
//   - HidsAlertManager：分级 + 去重 + 聚合 + SecurityMonitor 上报（串口通道）
//   - HidsNode：/proc/hids 状态节点（屏幕/网络通道）
//
// 复用 AuroraOS 特性：ProcFS、SecurityMonitor
// 设计原则（遵循 AGENTS.md）：组合优于继承、固定数组、零堆分配
// =============================================================================
#ifndef AURORA_HIDS_ENGINE_HPP
#define AURORA_HIDS_ENGINE_HPP

#include <stdint.h>
#include "../../kernel/core/mutex.hpp"
#include "../../kernel/core/security_monitor.hpp"
#include "../../vfs/vfs.hpp"
#include "../../vfs/procfs.hpp"
#include "file_integrity.hpp"
#include "process_monitor.hpp"
#include "privilege_auditor.hpp"
#include "rootkit_scanner.hpp"

// 系统 tick 计数器（定义于 boot/interrupts.cpp；宿主测试由 kernel_stubs.cpp 提供）
extern volatile uint32_t tick_count;

namespace aurora {
namespace hids {

// ---------------------------------------------------------------------------
// 严重度分级
// ---------------------------------------------------------------------------
enum class HidsSeverity : uint8_t {
    Info = 0,
    Low = 1,
    Medium = 2,
    High = 3,
    Critical = 4,
};

inline uint8_t hids_sev_rank(HidsSeverity s) {
    return static_cast<uint8_t>(s);
}

// ---------------------------------------------------------------------------
// 告警条目
// ---------------------------------------------------------------------------
struct HidsAlert {
    uint32_t timestamp_ms;
    HidsSeverity severity;
    uint32_t count;
    char source[24];
    char description[64];
};

// ---------------------------------------------------------------------------
// HidsAlertManager：分级 + 去重 + 聚合 + 上报
// ---------------------------------------------------------------------------
class HidsAlertManager {
public:
    static constexpr int kMaxAlerts = 32;
    static constexpr uint32_t kDedupWindowMs = 5000;
    static constexpr uint32_t kReportCooldownMs = 1000;
    static constexpr HidsSeverity kReportSeverity = HidsSeverity::High;

    void submit(HidsSeverity sev, const char* source, const char* desc) {
        const uint32_t now = tick_count;

        LockGuard lock(mutex_);

        // 1. 去重 + 聚合（按 source）
        const uint32_t total = alert_count_;
        const uint32_t start = (total > static_cast<uint32_t>(kMaxAlerts)) ? (total - kMaxAlerts) : 0;
        for (uint32_t i = start; i < total; ++i) {
            HidsAlert& a = alerts_[i % kMaxAlerts];
            if (!str_eq_(a.source, source))
                continue;
            if (now - a.timestamp_ms <= kDedupWindowMs) {
                ++a.count;
                a.timestamp_ms = now;
                if (hids_sev_rank(sev) > hids_sev_rank(a.severity))
                    a.severity = sev;
                maybe_report_(a);
                return;
            }
        }

        // 2. 新告警
        HidsAlert& slot = alerts_[alert_count_ % kMaxAlerts];
        ++alert_count_;
        slot.timestamp_ms = now;
        slot.severity = sev;
        slot.count = 1;
        copy_str_(slot.source, source, sizeof(slot.source));
        copy_str_(slot.description, desc, sizeof(slot.description));
        maybe_report_(slot);
    }

    uint32_t get_alert_count() const {
        return alert_count_;
    }

    const HidsAlert* get_alert(int index) const {
        if (index < 0)
            return nullptr;
        return &alerts_[static_cast<uint32_t>(index) % kMaxAlerts];
    }

    void reset() {
        LockGuard lock(mutex_);
        for (int i = 0; i < kMaxAlerts; ++i)
            alerts_[i] = HidsAlert{};
        alert_count_ = 0;
        last_report_ms_ = 0;
    }

private:
    HidsAlert alerts_[kMaxAlerts]{};
    uint32_t alert_count_ = 0;
    uint32_t last_report_ms_ = 0;
    Mutex mutex_;

    void maybe_report_(const HidsAlert& a) {
        if (hids_sev_rank(a.severity) < hids_sev_rank(kReportSeverity))
            return;
        if (tick_count - last_report_ms_ < kReportCooldownMs)
            return;
        last_report_ms_ = tick_count;
        SecurityMonitor::instance().report_firewall_anomaly(a.description);
    }

    static bool str_eq_(const char* a, const char* b) {
        while (*a && *b) {
            if (*a != *b)
                return false;
            ++a;
            ++b;
        }
        return *a == *b;
    }

    static void copy_str_(char* dst, const char* src, int maxlen) {
        int i = 0;
        while (src[i] && i < maxlen - 1) {
            dst[i] = src[i];
            ++i;
        }
        dst[i] = '\0';
    }
};

// ---------------------------------------------------------------------------
// ProcFS 节点: /proc/hids
// ---------------------------------------------------------------------------
class HidsNode : public ProcNode {
public:
    void set_engine(class HidsEngine* e) {
        engine_ = e;
    }

    int read(char* buf, int len, int offset, void* priv) override;

private:
    class HidsEngine* engine_ = nullptr;
};

// ---------------------------------------------------------------------------
// HidsEngine
// ---------------------------------------------------------------------------
class HidsEngine {
public:
    static HidsEngine& instance() {
        static HidsEngine engine;
        return engine;
    }

    void init() {
        if (initialized_)
            return;
        node_.set_engine(this);
        VfsManager::instance().mount("/proc/hids", &node_);
        initialized_ = true;
    }

    // 周期执行：驱动 4 个检测模块并将发现转为告警
    void tick() {
        scan_module_(file_integrity_, HidsSeverity::High);
        scan_module_(process_monitor_, HidsSeverity::Critical);
        scan_module_(privilege_auditor_, HidsSeverity::Critical);
        scan_module_(rootkit_scanner_, HidsSeverity::Critical);
    }

    // 配置/访问器
    FileIntegrityMonitor& file_integrity() {
        return file_integrity_;
    }

    ProcessMonitor& process_monitor() {
        return process_monitor_;
    }

    PrivilegeAuditor& privilege_auditor() {
        return privilege_auditor_;
    }

    RootkitScanner& rootkit_scanner() {
        return rootkit_scanner_;
    }

    uint32_t get_alert_count() const {
        return alerts_.get_alert_count();
    }

    const HidsAlert* get_alert(int index) const {
        return alerts_.get_alert(index);
    }

    void reset() {
        file_integrity_.reset();
        process_monitor_.reset();
        privilege_auditor_.reset();
        rootkit_scanner_.reset();
        alerts_.reset();
    }

private:
    HidsEngine() = default;

    FileIntegrityMonitor file_integrity_;
    ProcessMonitor process_monitor_;
    PrivilegeAuditor privilege_auditor_;
    RootkitScanner rootkit_scanner_;
    HidsAlertManager alerts_;
    HidsNode node_;
    bool initialized_ = false;

    template <typename M>
    void scan_module_(M& module, HidsSeverity sev) {
        const int findings = module.scan();
        if (findings > 0) {
            alerts_.submit(sev, module.get_name(), module.get_last_finding());
        }
    }

    friend class HidsNode;
};

// =============================================================================
// ProcFS 节点实现（header-only，与 wireless_ids.hpp 一致）
// =============================================================================
inline int HidsNode::read(char* buf, int len, int /*offset*/, void* /*priv*/) {
    if (!engine_)
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

    app_s("AuroraOS HIDS Status\n");
    app_s("===================\n");

    app_s("\n[file_integrity] findings=");
    app_u(engine_->file_integrity_.get_total_findings());
    app_s("\n[process_monitor] findings=");
    app_u(engine_->process_monitor_.get_total_findings());
    app_s(" overflow=");
    app_u(engine_->process_monitor_.get_overflow_count());
    app_s(" terminated=");
    app_u(engine_->process_monitor_.get_terminated_count());
    app_s("\n[privilege_auditor] findings=");
    app_u(engine_->privilege_auditor_.get_total_findings());
    app_s(" attempts=");
    app_u(engine_->privilege_auditor_.get_attempt_count());
    app_s("\n[rootkit_scanner] findings=");
    app_u(engine_->rootkit_scanner_.get_total_findings());
    app_s(" heap_corrupt=");
    app_u(static_cast<uint32_t>(engine_->rootkit_scanner_.get_heap_corrupt_count()));
    app_s("\n\nAlerts: ");
    app_u(engine_->get_alert_count());
    app_s("\n--- Recent Alerts ---\n");

    const uint32_t total = engine_->get_alert_count();
    const uint32_t start = (total > 16) ? (total - 16) : 0;
    for (uint32_t i = start; i < total && pos < len - 96; ++i) {
        const HidsAlert* a = engine_->get_alert(static_cast<int>(i));
        if (!a)
            continue;

        app_u(a->timestamp_ms);
        app_s(" ");
        app_s(a->source);
        app_s(" x");
        app_u(a->count);
        app_s(" [");
        app_u(static_cast<uint32_t>(a->severity));
        app_s("] ");
        app_s(a->description);
        app_s("\n");
    }

    buf[pos] = '\0';
    return pos;
}

} // namespace hids
} // namespace aurora

#endif // AURORA_HIDS_ENGINE_HPP
