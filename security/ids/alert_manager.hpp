// =============================================================================
// security/ids/alert_manager.hpp
//
// NIDS 告警管理：分级 / 去重 / 聚合 / 上报
//
//   - IdsCategory / IdsSeverity 定义告警类别与严重度分级
//   - AlertManager 维护固定环形缓冲，对同 (类别, src, dst) 在去重窗口内的
//     告警做聚合（计数累加、严重度升级），避免告警风暴
//   - 高严重度告警经冷却去抖后联动 SecurityMonitor（串口通道）
//
// 设计原则（遵循 AGENTS.md）：
//   - 固定环形缓冲，零动态内存分配
//   - Mutex 保护写入，读取为无锁快照（与 wireless_ids 一致）
//   - 屏幕 / 网络推送通道在引擎层通过 /proc/ids 与安全监控联动暴露
// =============================================================================
#ifndef AURORA_IDS_ALERT_MANAGER_HPP
#define AURORA_IDS_ALERT_MANAGER_HPP

#include <stdint.h>
#include "../../kernel/core/mutex.hpp"
#include "../../kernel/core/security_monitor.hpp"

// 系统 tick 计数器（定义于 boot/interrupts.cpp；宿主测试由 kernel_stubs.cpp 提供）
extern volatile uint32_t tick_count;

namespace aurora {
namespace ids {

// ---------------------------------------------------------------------------
// 告警类别
// ---------------------------------------------------------------------------
enum class IdsCategory : uint8_t {
    PortScan = 0,
    HostScan = 1,
    SynFlood = 2,
    ArpSpoof = 3,
    DnsTunnel = 4,
    MalformedPacket = 5,
    Fragmentation = 6,
    TcpFlagAnomaly = 7,
    PayloadSignature = 8,
    TrafficAnomaly = 9,
};

// ---------------------------------------------------------------------------
// 严重度分级
// ---------------------------------------------------------------------------
enum class IdsSeverity : uint8_t {
    Info = 0,
    Low = 1,
    Medium = 2,
    High = 3,
    Critical = 4,
};

inline uint8_t sev_rank(IdsSeverity s) {
    return static_cast<uint8_t>(s);
}

// ---------------------------------------------------------------------------
// 告警条目
// ---------------------------------------------------------------------------
struct IdsAlert {
    uint32_t timestamp_ms;
    IdsCategory category;
    IdsSeverity severity;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t count; // 聚合计数
    char description[64];
};

// ---------------------------------------------------------------------------
// AlertManager
// ---------------------------------------------------------------------------
class AlertManager {
public:
    static constexpr int kMaxAlerts = 64;
    static constexpr uint32_t kDedupWindowMs = 5000;
    static constexpr IdsSeverity kReportSeverity = IdsSeverity::High;
    static constexpr uint32_t kReportCooldownMs = 1000;

    // 提交一条告警：去重 + 聚合 + 分级 + 存储 + 按需上报。
    // 返回存储槽位指针（新告警或聚合后的告警），供上层查询/展示。
    const IdsAlert* submit(IdsCategory cat, IdsSeverity sev, uint32_t src, uint32_t dst,
                           uint16_t sport, uint16_t dport, const char* desc) {
        const uint32_t now = tick_count;

        LockGuard lock(alert_mutex_);

        // 1. 去重 + 聚合：同 (类别, src, dst) 在窗口内 → 计数累加 + 严重度升级
        const uint32_t total = alert_count_;
        const uint32_t start = (total > static_cast<uint32_t>(kMaxAlerts)) ? (total - kMaxAlerts) : 0;
        for (uint32_t i = start; i < total; ++i) {
            IdsAlert& a = alerts_[i % kMaxAlerts];
            if (a.category != cat || a.src_ip != src || a.dst_ip != dst)
                continue;
            if (now - a.timestamp_ms <= kDedupWindowMs) {
                ++a.count;
                a.timestamp_ms = now;
                if (sev_rank(sev) > sev_rank(a.severity))
                    a.severity = sev;
                maybe_report_(a);
                return &a;
            }
        }

        // 2. 新告警：覆盖最老槽位
        IdsAlert& slot = alerts_[alert_count_ % kMaxAlerts];
        ++alert_count_;

        slot.timestamp_ms = now;
        slot.category = cat;
        slot.severity = sev;
        slot.src_ip = src;
        slot.dst_ip = dst;
        slot.src_port = sport;
        slot.dst_port = dport;
        slot.count = 1;
        fill_desc_(slot, desc);
        maybe_report_(slot);
        return &slot;
    }

    // 历史告警总数（单调递增）
    uint32_t get_alert_count() const {
        return alert_count_;
    }

    // 按写入序号读取告警（环形缓冲）
    const IdsAlert* get_alert(int index) const {
        if (index < 0)
            return nullptr;
        return &alerts_[static_cast<uint32_t>(index) % kMaxAlerts];
    }

    void reset() {
        LockGuard lock(alert_mutex_);
        for (int i = 0; i < kMaxAlerts; ++i)
            alerts_[i] = IdsAlert{};
        alert_count_ = 0;
        last_report_ms_ = 0;
    }

    static const char* category_name(IdsCategory c) {
        switch (c) {
        case IdsCategory::PortScan:
            return "port_scan";
        case IdsCategory::HostScan:
            return "host_scan";
        case IdsCategory::SynFlood:
            return "syn_flood";
        case IdsCategory::ArpSpoof:
            return "arp_spoof";
        case IdsCategory::DnsTunnel:
            return "dns_tunnel";
        case IdsCategory::MalformedPacket:
            return "malformed_packet";
        case IdsCategory::Fragmentation:
            return "fragmentation";
        case IdsCategory::TcpFlagAnomaly:
            return "tcp_flag_anomaly";
        case IdsCategory::PayloadSignature:
            return "payload_signature";
        case IdsCategory::TrafficAnomaly:
            return "traffic_anomaly";
        default:
            return "unknown";
        }
    }

private:
    IdsAlert alerts_[kMaxAlerts]{};
    uint32_t alert_count_ = 0;
    uint32_t last_report_ms_ = 0;
    Mutex alert_mutex_;

    // 高严重度且满足冷却窗口才联动 SecurityMonitor，避免 UART 告警风暴
    void maybe_report_(const IdsAlert& a) {
        if (sev_rank(a.severity) < sev_rank(kReportSeverity))
            return;
        if (tick_count - last_report_ms_ < kReportCooldownMs)
            return;
        last_report_ms_ = tick_count;
        SecurityMonitor::instance().report_firewall_anomaly(a.description);
    }

    static void fill_desc_(IdsAlert& a, const char* desc) {
        char* p = a.description;
        const char* const end = a.description + sizeof(a.description) - 1;

        const char* cat = category_name(a.category);
        while (*cat && p < end)
            *p++ = *cat++;
        if (p < end)
            *p++ = ':';
        if (p < end)
            *p++ = ' ';
        while (*desc && p < end)
            *p++ = *desc++;
        *p = '\0';
    }
};

} // namespace ids
} // namespace aurora

#endif // AURORA_IDS_ALERT_MANAGER_HPP
