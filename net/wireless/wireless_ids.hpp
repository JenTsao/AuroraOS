#ifndef AURORA_WIRELESS_IDS_HPP
#define AURORA_WIRELESS_IDS_HPP

#include <stdint.h>
#include <stddef.h>
#include "wifi_monitor.hpp"
#include "beacon_analyzer.hpp"
#include "deauth_detector.hpp"
#include "../../kernel/mutex.hpp"
#include "../../kernel/task.hpp"
#include "../../kernel/security_monitor.hpp"
#include "../../vfs/vfs.hpp"

// ============================================================
// Wireless IDS — 无线入侵检测规则引擎
//
// 参考 FirewallEngine + RuleTable 模式设计。
// 集成 BeaconAnalyzer + DeauthDetector + HandshakeCapture，
// 通过规则引擎统一评分并生成安全告警。
//
// 检测的攻击类型：
//   - Deauth Flood (解除认证洪水)
//   - Evil Twin (伪造同名 AP)
//   - Weak Encryption (弱加密/WEP)
//   - Hidden SSID Abuse (隐藏 SSID 扫描)
//   - KARMA Attack (响应任意 Probe Request)
//   - KRACK (四次握手重放)
//   - PMKID Capture (无客户端握手捕获)
//   - WPS PIN Bruteforce
//   - Channel Hopping Anomaly
// ============================================================

// 无线 IDS 事件（供规则引擎消费的标准化事件）
struct WirelessEvent {
    uint32_t timestamp;
    uint8_t  bssid[6];
    uint8_t  station[6];        // 关联的 STA MAC（可选，全零=无）
    uint8_t  event_type;        // WirelessEventType
    uint8_t  severity;          // 0-100
    uint16_t param0;            // 上下文参数（计数/信道/原因码等）
    int8_t   rssi_dbm;
};

// 事件类型
enum class WirelessEventType : uint8_t {
    DeauthFlood      = 0,   // Deauth 洪水
    EvilTwin         = 1,   // 同名 AP 不同 BSSID
    WeakEncryption   = 2,   // 弱加密
    HiddenSsid       = 3,   // 隐藏 SSID
    SpoofedFrame     = 4,   // 欺骗帧
    HandshakeCapture = 5,   // 握手捕获（渗透行为）
    ChannelHop       = 6,   // 异常信道跳变
    WpsDetected      = 7,   // WPS 启用
    RogueProbeResp   = 8,   // 恶意 Probe Response
    UnknownFrameType = 9,   // 未知帧类型
};

// IDS 规则
struct IdsRule {
    WirelessEventType event_type;  // 匹配事件类型
    uint8_t  min_severity;         // 最低严重度（0-100）
    uint16_t threshold_count;      // 窗口内计数阈值
    uint32_t window_ms;            // 时间窗口
    uint8_t  action;               // 0=记录, 1=告警, 2=SecurityMonitor, 3=终止可疑任务
    bool     enabled;
};

// 安全告警
struct SecurityAlert {
    uint32_t       timestamp;
    WirelessEventType alert_type;
    uint8_t        bssid[6];
    uint8_t        severity;
    char           description[128];
    uint16_t       event_count;
};

// ============================================================
// ProcFS 节点: /proc/wireless_ids
// ============================================================
class WirelessIdsNode : public ProcNode {
public:
    void set_engine(class WirelessIds* engine) { engine_ = engine; }

    int read(char* buf, int len, int offset, void* /*priv*/) override;

private:
    class WirelessIds* engine_ = nullptr;
};

// ============================================================
// Wireless IDS 引擎主类
// ============================================================
class WirelessIds {
public:
    static WirelessIds& instance() {
        static WirelessIds ids;
        return ids;
    }

    // ---- 初始化 ----

    void init() {
        if (initialized_) return;
        ids_node_.set_engine(this);
        VfsManager::instance().mount("/proc/wireless_ids", &ids_node_);
        initialized_ = true;
    }

    // ---- 规则管理（参考 firewall/rule_table.hpp） ----

    static constexpr int kMaxRules = 32;

    bool add_rule(const IdsRule& rule) {
        for (int i = 0; i < kMaxRules; ++i) {
            if (!rules_[i].enabled) {
                rules_[i] = rule;
                rules_[i].enabled = true;
                return true;
            }
        }
        return false;
    }

    void remove_rule(int index) {
        if (index >= 0 && index < kMaxRules) rules_[index].enabled = false;
    }

    void clear_rules() {
        for (int i = 0; i < kMaxRules; ++i) rules_[i].enabled = false;
    }

    int get_rule_count() const {
        int count = 0;
        for (int i = 0; i < kMaxRules; ++i) {
            if (rules_[i].enabled) ++count;
        }
        return count;
    }

    const IdsRule* get_rule(int index) const {
        if (index >= 0 && index < kMaxRules) {
            return &rules_[index];
        }
        return nullptr;
    }

    // ---- 事件提交（由 BeaconAnalyzer / DeauthDetector 调用） ----

    void submit_event(WirelessEventType type, const uint8_t* bssid,
                      const uint8_t* station, int8_t rssi, uint16_t param) {
        WirelessEvent ev{};
        ev.timestamp = get_tick_();
        ev.event_type = static_cast<uint8_t>(type);
        for (int i = 0; i < 6; ++i) {
            if (bssid) ev.bssid[i] = bssid[i];
            if (station) ev.station[i] = station[i];
        }
        ev.rssi_dbm = rssi;
        ev.param0 = param;

        // 基础严重度
        ev.severity = baseline_severity_(type);

        LockGuard lock(event_mutex_);
        store_event_(ev);
        evaluate_rules_(ev);
    }

    // 快速提交（无 Station）
    void submit_event_simple(WirelessEventType type, const uint8_t* bssid,
                             int8_t rssi, uint16_t param) {
        submit_event(type, bssid, nullptr, rssi, param);
    }

    // ---- 攻击扫描整合 ----

    // 对 BeaconAnalyzer 的 AP 表运行安全检查
    void scan_beacons() {
        BeaconAnalyzer& beacon = BeaconAnalyzer::instance();

        // 弱加密检测
        for (int i = 0; i < beacon.get_ap_count(); ++i) {
            const ApInfo* ap = beacon.get_ap(i);
            if (!ap) continue;
            if (BeaconAnalyzer::is_weak_encryption(ap->encryption)) {
                submit_event_simple(WirelessEventType::WeakEncryption,
                                    ap->bssid, ap->rssi_dbm, ap->channel);
            }
        }

        // 隐藏 SSID 检测
        int hidden = beacon.count_hidden_ssid();
        if (hidden > 0) {
            for (int i = 0; i < beacon.get_ap_count(); ++i) {
                const ApInfo* ap = beacon.get_ap(i);
                if (ap && ap->is_hidden) {
                    submit_event_simple(WirelessEventType::HiddenSsid,
                                        ap->bssid, ap->rssi_dbm, ap->channel);
                }
            }
        }

        // Evil Twin 检测
        BeaconAnalyzer::RogueApAlert rogue_alerts[16];
        int rogue_count = beacon.detect_rogue_aps(rogue_alerts, 16);
        for (int i = 0; i < rogue_count; ++i) {
            submit_event(WirelessEventType::EvilTwin,
                         rogue_alerts[i].bssid_b, rogue_alerts[i].bssid_a,
                         0, rogue_alerts[i].channel_b);
        }
    }

    // 对 DeauthDetector 的跟踪数据运行安全检查
    void scan_deauths() {
        DeauthDetector& deauth = DeauthDetector::instance();

        uint8_t recent_bssids[64 * 6];
        int recent = deauth.get_recent_attacks(10, recent_bssids, 64);
        for (int i = 0; i < recent; ++i) {
            submit_event_simple(WirelessEventType::DeauthFlood,
                                &recent_bssids[i * 6], 0, 0);
        }
    }

    // ---- 告警管理 ----

    int get_alert_count() const { return alert_count_; }

    const SecurityAlert* get_alert(int index) const {
        if (index < 0 || index >= alert_count_) return nullptr;
        return &alerts_[index];
    }

    int get_event_count() const {
        int total = event_count_;
        return (total > kMaxEvents) ? kMaxEvents : total;
    }

    const WirelessEvent* get_event(int index) const {
        if (index < 0 || index >= kMaxEvents) return nullptr;
        return &event_ring_[index];
    }

    // ---- 统计 ----

    uint32_t get_total_events() const { return event_count_; }
    uint32_t get_total_alerts() const { return alert_generated_; }

    // ---- 预置规则 ----

    void load_default_rules() {
        // R1: Deauth Flood → Critical
        IdsRule r1{WirelessEventType::DeauthFlood, 60, 15, 5000, 2, true};
        add_rule(r1);

        // R2: Evil Twin → Alert
        IdsRule r2{WirelessEventType::EvilTwin, 80, 1, 10000, 1, true};
        add_rule(r2);

        // R3: 弱加密 → Warning
        IdsRule r3{WirelessEventType::WeakEncryption, 40, 1, 60000, 1, true};
        add_rule(r3);

        // R4: 欺骗帧 → Alert
        IdsRule r4{WirelessEventType::SpoofedFrame, 70, 5, 3000, 2, true};
        add_rule(r4);

        // R5: 握手捕获 → Info（可能是合法渗透测试）
        IdsRule r5{WirelessEventType::HandshakeCapture, 30, 3, 60000, 0, true};
        add_rule(r5);

        // R6: WPS 启用 → Warning
        IdsRule r6{WirelessEventType::WpsDetected, 35, 1, 60000, 1, true};
        add_rule(r6);

        // R7: 未知帧类型 → Alert（可能是攻击工具自定义帧）
        IdsRule r7{WirelessEventType::UnknownFrameType, 50, 3, 10000, 1, true};
        add_rule(r7);
    }

    // ---- 工具方法 ----

    static const char* event_type_name(WirelessEventType t) {
        switch (t) {
            case WirelessEventType::DeauthFlood:      return "deauth_flood";
            case WirelessEventType::EvilTwin:         return "evil_twin";
            case WirelessEventType::WeakEncryption:   return "weak_encryption";
            case WirelessEventType::HiddenSsid:       return "hidden_ssid";
            case WirelessEventType::SpoofedFrame:     return "spoofed_frame";
            case WirelessEventType::HandshakeCapture: return "handshake_capture";
            case WirelessEventType::ChannelHop:       return "channel_hop";
            case WirelessEventType::WpsDetected:      return "wps_detected";
            case WirelessEventType::RogueProbeResp:   return "rogue_probe_resp";
            case WirelessEventType::UnknownFrameType: return "unknown_frame";
            default:                                  return "?";
        }
    }

private:
    static constexpr int kMaxEvents = 256;
    static constexpr int kMaxAlerts = 64;

    WirelessEvent event_ring_[kMaxEvents]{};
    volatile int  event_count_ = 0;
    Mutex         event_mutex_;

    SecurityAlert alerts_[kMaxAlerts]{};
    int           alert_count_ = 0;
    Mutex         alert_mutex_;
    uint32_t      alert_generated_ = 0;

    IdsRule       rules_[kMaxRules]{};
    bool          initialized_ = false;
    WirelessIdsNode ids_node_;

    WirelessIds() = default;

    void store_event_(const WirelessEvent& ev) {
        int idx = event_count_ % kMaxEvents;
        event_ring_[idx] = ev;
        ++event_count_;
    }

    void evaluate_rules_(const WirelessEvent& ev) {
        for (int i = 0; i < kMaxRules; ++i) {
            const IdsRule& rule = rules_[i];
            if (!rule.enabled) continue;
            if (rule.event_type != static_cast<WirelessEventType>(ev.event_type)) continue;
            if (ev.severity < rule.min_severity) continue;

            // 阈值检查
            if (rule.threshold_count > 0) {
                uint32_t window_start = ev.timestamp - rule.window_ms;
                int match_count = 0;
                int limit = event_count_;
                if (limit > kMaxEvents) limit = kMaxEvents;
                for (int j = 0; j < limit; ++j) {
                    int idx = (event_count_ - 1 - j) % kMaxEvents;
                    if (idx < 0) idx += kMaxEvents;
                    const WirelessEvent& hist = event_ring_[idx];
                    if (hist.timestamp < window_start) break;
                    if (hist.event_type == ev.event_type) ++match_count;
                }
                if (match_count < rule.threshold_count) continue;
            }

            // 生成告警
            SecurityAlert alert{};
            alert.timestamp  = ev.timestamp;
            alert.alert_type = static_cast<WirelessEventType>(ev.event_type);
            for (int j = 0; j < 6; ++j) alert.bssid[j] = ev.bssid[j];
            alert.severity    = ev.severity;
            alert.event_count = static_cast<uint16_t>(ev.param0);

            // 告警描述
            int dpos = 0;
            auto desc_app = [&](const char* s) {
                while (*s && dpos < 127) alert.description[dpos++] = *s++;
            };
            desc_app(event_type_name(static_cast<WirelessEventType>(ev.event_type)));
            desc_app(" BSSID=");
            for (int j = 0; j < 6 && dpos < 127 - 3; ++j) {
                uint8_t b = ev.bssid[j];
                char h = (b >> 4) & 0xF;
                char l = b & 0xF;
                if (dpos < 127) alert.description[dpos++] = h < 10 ? '0'+h : 'A'+h-10;
                if (dpos < 127) alert.description[dpos++] = l < 10 ? '0'+l : 'A'+l-10;
                if (j < 5 && dpos < 127) alert.description[dpos++] = ':';
            }
            alert.description[dpos] = '\0';

            // 存储告警
            {
                LockGuard alock(alert_mutex_);
                alerts_[alert_count_ % kMaxAlerts] = alert;
                ++alert_count_;
                ++alert_generated_;
            }

            // 执行规则动作
            if (rule.action >= 2) {
                SecurityMonitor::instance().report_firewall_anomaly(
                    alert.description);
            }
        }
    }

    static uint8_t baseline_severity_(WirelessEventType type) {
        switch (type) {
            case WirelessEventType::DeauthFlood:      return 90;
            case WirelessEventType::EvilTwin:         return 85;
            case WirelessEventType::SpoofedFrame:     return 80;
            case WirelessEventType::RogueProbeResp:   return 75;
            case WirelessEventType::HandshakeCapture: return 50;
            case WirelessEventType::WeakEncryption:   return 60;
            case WirelessEventType::HiddenSsid:       return 20;
            case WirelessEventType::WpsDetected:      return 40;
            case WirelessEventType::ChannelHop:       return 30;
            case WirelessEventType::UnknownFrameType: return 55;
            default:                                  return 10;
        }
    }

    static uint32_t get_tick_() {
        extern volatile uint32_t tick_count;
        return tick_count;
    }

    friend class WirelessIdsNode;
};

// ============================================================
// ProcFS 节点实现
// ============================================================
inline int WirelessIdsNode::read(char* buf, int len, int offset, void* /*priv*/) {
    if (!engine_) return 0;

    int pos = 0;
    auto app_s = [&](const char* s) {
        while (*s && pos < len - 1) buf[pos++] = *s++;
    };
    auto app_u = [&](uint32_t num) {
        char tmp[16]; int i = 0;
        if (num == 0) { tmp[i++] = '0'; }
        while (num > 0) { tmp[i++] = '0' + (num % 10); num /= 10; }
        while (i > 0 && pos < len - 1) buf[pos++] = tmp[--i];
    };

    app_s("Wireless IDS Status\n");
    app_s("===================\n");

    app_s("Total Events: "); app_u(engine_->get_total_events()); app_s("\n");
    app_s("Total Alerts: "); app_u(engine_->get_total_alerts()); app_s("\n");
    app_s("Active Rules: "); app_u(engine_->get_rule_count()); app_s("\n");
    app_s("\n--- Recent Alerts ---\n");

    int alert_total = engine_->get_alert_count();
    int start = (alert_total > 20) ? alert_total - 20 : 0;
    for (int i = start; i < alert_total && pos < len - 80; ++i) {
        const SecurityAlert* a = engine_->get_alert(i % WirelessIds::kMaxAlerts);
        if (!a) continue;

        app_u(a->timestamp); app_s(" ");
        app_s(WirelessIds::event_type_name(a->alert_type));
        app_s(" ["); app_u(a->severity); app_s("] ");
        app_s(a->description);
        app_s("\n");
    }

    buf[pos] = '\0';
    return pos;
}

#endif // AURORA_WIRELESS_IDS_HPP
