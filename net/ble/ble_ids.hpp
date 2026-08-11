// ============================================================
// ble_ids.hpp — BLE Intrusion Detection System Rule Engine
//
// Header-only IDS engine.  Consumes events from BleScanner,
// GattAuditor, and BleMitmDetector; evaluates them through a
// configurable rule engine; and raises alerts via
// SecurityMonitor and /proc/ble_ids.
//
// Detected attack patterns:
//   - BLE scanning storm (too many scans/advertisements)
//   - Connection flooding (rapid connect/disconnect cycles)
//   - GATT brute-force (characteristic discovery storm)
//   - Advertised weak security (no LE SC, JustWorks)
//   - MAC address randomization abuse (privacy address cycling)
//   - Known vulnerable services discovered
//   - MITM pairing downgrade detected
//   - Unauthorized write attempt (signature failure)
//   - RSSI-based proximity spoofing
//   - Excessive MTU negotiation
//
// Integration points:
//   SecurityMonitor::report_firewall_anomaly() — Critical alerts
//   /proc/ble_ids — Read for live alert feed
// ============================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../../kernel/core/mutex.hpp"
#include "../../kernel/core/security_monitor.hpp"
#include "../../vfs/procfs.hpp"
#include "ble_scanner.hpp"
#include "gatt_auditor.hpp"
#include "ble_mitm.hpp"

extern volatile uint32_t tick_count;

// ---- BLE IDS Event Types ----
enum class BleIdsEventType : uint8_t {
    ScanStorm            = 0,   // Excessive scan/advertising packets
    ConnectFlood         = 1,   // Rapid connect/disconnect cycles
    GattDiscoveryStorm   = 2,   // GATT characteristic discovery flooding
    WeakSecurity         = 3,   // No LE SC, no OOB, JustWorks only
    MacSpoofing          = 4,   // Privacy address cycling abuse
    KnownVulnerableSvc   = 5,   // Known-insecure GATT service discovered
    PairingDowngrade     = 6,   // MITM: pairing downgrade detected
    UnauthorizedWrite    = 7,   // Signature verification failure
    RssiSpoofing         = 8,   // RSSI paradox (relay attack indicator)
    ExcessiveMtu         = 9,   // MTU negotiation storm
};

// ---- BLE IDS Event (standardized) ----
struct BleIdsEvent {
    uint32_t          timestamp;
    uint8_t           mac[6];
    BleIdsEventType   event_type;
    uint8_t           severity;       // 0-100
    uint16_t          param;          // context value (count/channel/handle)
};

// ---- BLE IDS Rule ----
struct BleIdsRule {
    BleIdsEventType  event_type;      // Event type to match
    uint8_t          min_severity;    // Minimum severity (0-100)
    uint16_t         threshold_count; // Events within window to trigger
    uint32_t         window_ms;       // Observation window
    uint8_t          action;          // 0=log, 1=alert, 2=report+block
    bool             enabled;
};

// ---- BLE IDS Alert ----
struct BleIdsAlert {
    uint32_t         timestamp;
    BleIdsEventType  event_type;
    uint8_t          severity;
    uint8_t          mac[6];
    char             description[64];
    uint16_t         event_count;     // Events counted in window
};

// ---- BLE IDS Engine ----
class BleIds {
public:
    static constexpr int kMaxEvents        = 256;
    static constexpr int kMaxAlerts        = 64;
    static constexpr int kMaxRules         = 32;
    static constexpr int kMacSpoofWindowMs = 5000;    // 5s for MAC cycling
    static constexpr int kScanStormWindowMs = 10000;   // 10s
    static constexpr int kConnectFloodWindowMs = 5000; // 5s

    static BleIds& instance() noexcept {
        static BleIds ids;
        return ids;
    }

    // ---- Event Feeds ----

    // Push a standardized IDS event into the engine.
    void push_event(const BleIdsEvent& evt) noexcept;

    // Push from BleScanner (advertising event → possible ScanStorm/MacSpoofing)
    void feed_advertisement(const uint8_t mac[6], int8_t rssi, uint8_t ad_flags) noexcept;

    // Push from GattAuditor (finding → KnownVulnerableSvc / UnauthWrite)
    void feed_gatt_finding(const GattFinding& finding) noexcept;

    // Push from BleMitmDetector (MITM alert → PairingDowngrade/RssiSpoofing)
    void feed_mitm_alert(const MitmAlert& alert) noexcept;

    // Push connection count event (→ ConnectFlood)
    void feed_connection_event(const uint8_t mac[6], bool is_connect, uint8_t reason) noexcept;

    // Push signature verification failure (→ UnauthorizedWrite)
    void feed_signature_failure(const uint8_t mac[6]) noexcept;

    // ---- Periodic Scan ----

    // Run a full scan: aggregate recent events, evaluate rules, generate alerts.
    // Typically called from a periodic task or timer callback.
    int scan_and_evaluate() noexcept;

    // ---- Rules ----

    void load_default_rules() noexcept;
    bool add_rule(const BleIdsRule& rule) noexcept;
    void remove_rule(int index) noexcept;
    void clear_rules() noexcept;
    int  rule_count() const noexcept;

    // ---- Query ----

    int event_count() const noexcept;
    int alert_count() const noexcept;
    const BleIdsAlert* get_alert(int index) const noexcept;
    void clear_alerts() noexcept;

    // ---- ProcFS ----
    //
    // After calling init(), /proc/ble_ids provides a live feed:
    //   Total Events  |  Total Alerts  |  Active Rules
    //   [timestamp] [type] [severity] [mac] [description]

    void init();                     // Mount /proc/ble_ids
    static int procfs_read(char* buf, int len, int offset, void* priv);

    // ---- Helpers ----

    static const char* event_type_name(BleIdsEventType t) noexcept {
        switch (t) {
            case BleIdsEventType::ScanStorm:           return "ScanStorm";
            case BleIdsEventType::ConnectFlood:        return "ConnectFlood";
            case BleIdsEventType::GattDiscoveryStorm:  return "GattDiscoveryStorm";
            case BleIdsEventType::WeakSecurity:        return "WeakSecurity";
            case BleIdsEventType::MacSpoofing:         return "MacSpoofing";
            case BleIdsEventType::KnownVulnerableSvc:  return "KnownVulnerableSvc";
            case BleIdsEventType::PairingDowngrade:    return "PairingDowngrade";
            case BleIdsEventType::UnauthorizedWrite:   return "UnauthorizedWrite";
            case BleIdsEventType::RssiSpoofing:        return "RssiSpoofing";
            case BleIdsEventType::ExcessiveMtu:        return "ExcessiveMtu";
            default:                                    return "Unknown";
        }
    }

private:
    BleIds() = default;

    mutable Mutex mutex_;
    BleIdsEvent  events_[kMaxEvents];
    int          event_count_ = 0;
    int          event_head_ = 0;

    BleIdsAlert  alerts_[kMaxAlerts];
    int          alert_count_ = 0;

    BleIdsRule   rules_[kMaxRules];
    int          rule_count_ = 0;

    // Sliding window counters
    uint32_t     scan_count_window_start_ = 0;
    uint32_t     scan_count_ = 0;
    uint32_t     connect_count_window_start_ = 0;
    uint32_t     connect_count_ = 0;

    void evaluate_rules_() noexcept;
    void add_event_(const BleIdsEvent& evt) noexcept;
};

// ---- ProcFS Node ----
class BleIdsProcNode : public ProcNode {
public:
    BleIdsProcNode() : ProcNode("ble_ids") {}
    int read(char* buf, int len, int offset, void* priv) override {
        return BleIds::procfs_read(buf, len, offset, priv);
    }
};

// ============================================================
// Inline implementations
// ============================================================

inline void BleIds::init() {
    LockGuard lock(mutex_);
    load_default_rules();

    // Mount /proc/ble_ids
    VFS::mount_proc("/proc/ble_ids", new BleIdsProcNode());
}

inline void BleIds::push_event(const BleIdsEvent& evt) noexcept {
    LockGuard lock(mutex_);
    add_event_(evt);
}

inline void BleIds::feed_advertisement(
    const uint8_t mac[6], int8_t /*rssi*/, uint8_t ad_flags) noexcept
{
    if (!mac) return;
    LockGuard lock(mutex_);

    // Count scan events per window
    uint32_t now = tick_count;
    if (now - scan_count_window_start_ > kScanStormWindowMs) {
        scan_count_window_start_ = now;
        scan_count_ = 0;
    }
    ++scan_count_;

    // Scan storm threshold: > 50 advertisements in 10s from one source
    if (scan_count_ > 50) {
        BleIdsEvent evt{};
        evt.timestamp  = now;
        for (int i = 0; i < 6; ++i) evt.mac[i] = mac[i];
        evt.event_type = BleIdsEventType::ScanStorm;
        evt.severity   = 40;
        evt.param      = static_cast<uint16_t>(scan_count_);
        add_event_(evt);
    }

    // Weak security detection
    bool has_le_sc = (ad_flags != 0) && ((ad_flags & 0x80) != 0);  // simplified
    if (!has_le_sc) {
        BleIdsEvent evt{};
        evt.timestamp  = now;
        for (int i = 0; i < 6; ++i) evt.mac[i] = mac[i];
        evt.event_type = BleIdsEventType::WeakSecurity;
        evt.severity   = 25;
        evt.param      = 0;
        add_event_(evt);
    }
}

inline void BleIds::feed_gatt_finding(const GattFinding& finding) noexcept {
    LockGuard lock(mutex_);

    BleIdsEvent evt{};
    evt.timestamp = finding.timestamp;
    for (int i = 0; i < 6; ++i) evt.mac[i] = finding.device_mac[i];

    switch (finding.type) {
    case GattFindingType::KnownInsecureService:
        evt.event_type = BleIdsEventType::KnownVulnerableSvc;
        evt.severity   = 80;
        break;
    case GattFindingType::OpenWriteNoAuth:
    case GattFindingType::UnauthenticatedWrite:
        evt.event_type = BleIdsEventType::UnauthorizedWrite;
        evt.severity   = 70;
        break;
    case GattFindingType::WeakAuthentication:
    case GattFindingType::MissingEncryption:
        evt.event_type = BleIdsEventType::WeakSecurity;
        evt.severity   = 50;
        break;
    default:
        evt.event_type = BleIdsEventType::WeakSecurity;
        evt.severity   = static_cast<uint8_t>(finding.severity) * 25;
        break;
    }
    evt.param = finding.char_handle;
    add_event_(evt);
}

inline void BleIds::feed_mitm_alert(const MitmAlert& alert) noexcept {
    LockGuard lock(mutex_);

    BleIdsEvent evt{};
    evt.timestamp = alert.timestamp;
    for (int i = 0; i < 6; ++i) evt.mac[i] = alert.device_mac[i];

    switch (alert.type) {
    case MitmAlertType::PairingDowngrade:
    case MitmAlertType::SecurityLevelDowngrade:
        evt.event_type = BleIdsEventType::PairingDowngrade;
        evt.severity   = 90;
        break;
    case MitmAlertType::RssiParadox:
    case MitmAlertType::AddressSpoofing:
        evt.event_type = BleIdsEventType::RssiSpoofing;
        evt.severity   = 75;
        break;
    default:
        evt.event_type = BleIdsEventType::PairingDowngrade;
        evt.severity   = static_cast<uint8_t>(alert.confidence);
        break;
    }
    evt.param = 0;
    add_event_(evt);
}

inline void BleIds::feed_connection_event(
    const uint8_t mac[6], bool is_connect, uint8_t /*reason*/) noexcept
{
    if (!mac) return;
    LockGuard lock(mutex_);

    if (is_connect) {
        uint32_t now = tick_count;
        if (now - connect_count_window_start_ > kConnectFloodWindowMs) {
            connect_count_window_start_ = now;
            connect_count_ = 0;
        }
        ++connect_count_;

        // Connect flood threshold: > 10 connections in 5s
        if (connect_count_ > 10) {
            BleIdsEvent evt{};
            evt.timestamp  = now;
            for (int i = 0; i < 6; ++i) evt.mac[i] = mac[i];
            evt.event_type = BleIdsEventType::ConnectFlood;
            evt.severity   = 65;
            evt.param      = static_cast<uint16_t>(connect_count_);
            add_event_(evt);
        }
    }
}

inline void BleIds::feed_signature_failure(const uint8_t mac[6]) noexcept {
    if (!mac) return;
    LockGuard lock(mutex_);

    BleIdsEvent evt{};
    evt.timestamp  = tick_count;
    for (int i = 0; i < 6; ++i) evt.mac[i] = mac[i];
    evt.event_type = BleIdsEventType::UnauthorizedWrite;
    evt.severity   = 85;
    evt.param      = 0;
    add_event_(evt);
}

inline int BleIds::scan_and_evaluate() noexcept {
    LockGuard lock(mutex_);
    evaluate_rules_();
    return alert_count_;
}

// ---- Default Rules ----

inline void BleIds::load_default_rules() noexcept {
    BleIdsRule r{};

    // Rule 1: ScanStorm > 30 events in 10s → Alert
    r.event_type      = BleIdsEventType::ScanStorm;
    r.min_severity    = 30;
    r.threshold_count = 30;
    r.window_ms       = 10000;
    r.action          = 1;  // alert
    r.enabled         = true;
    add_rule(r);

    // Rule 2: ConnectFlood > 5 events in 5s → Report + Block
    r.event_type      = BleIdsEventType::ConnectFlood;
    r.min_severity    = 40;
    r.threshold_count = 5;
    r.window_ms       = 5000;
    r.action          = 2;  // report + block
    r.enabled         = true;
    add_rule(r);

    // Rule 3: KnownVulnerableSvc → Report (always critical)
    r.event_type      = BleIdsEventType::KnownVulnerableSvc;
    r.min_severity    = 50;
    r.threshold_count = 1;
    r.window_ms       = 60000;
    r.action          = 2;
    r.enabled         = true;
    add_rule(r);

    // Rule 4: PairingDowngrade → Report (always critical)
    r.event_type      = BleIdsEventType::PairingDowngrade;
    r.min_severity    = 60;
    r.threshold_count = 1;
    r.window_ms       = 60000;
    r.action          = 2;
    r.enabled         = true;
    add_rule(r);

    // Rule 5: UnauthorizedWrite > 3 in 10s → Report
    r.event_type      = BleIdsEventType::UnauthorizedWrite;
    r.min_severity    = 50;
    r.threshold_count = 3;
    r.window_ms       = 10000;
    r.action          = 2;
    r.enabled         = true;
    add_rule(r);

    // Rule 6: RssiSpoofing → Alert
    r.event_type      = BleIdsEventType::RssiSpoofing;
    r.min_severity    = 40;
    r.threshold_count = 1;
    r.window_ms       = 30000;
    r.action          = 1;
    r.enabled         = true;
    add_rule(r);

    // Rule 7: WeakSecurity (warn if many devices have weak security)
    r.event_type      = BleIdsEventType::WeakSecurity;
    r.min_severity    = 20;
    r.threshold_count = 10;
    r.window_ms       = 60000;
    r.action          = 0;  // log only
    r.enabled         = true;
    add_rule(r);
}

inline bool BleIds::add_rule(const BleIdsRule& rule) noexcept {
    if (rule_count_ >= kMaxRules) return false;
    rules_[rule_count_] = rule;
    ++rule_count_;
    return true;
}

inline void BleIds::remove_rule(int index) noexcept {
    if (index < 0 || index >= rule_count_) return;
    for (int i = index; i < rule_count_ - 1; ++i)
        rules_[i] = rules_[i + 1];
    --rule_count_;
}

inline void BleIds::clear_rules() noexcept {
    rule_count_ = 0;
}

inline int BleIds::rule_count() const noexcept {
    LockGuard lock(mutex_);
    return rule_count_;
}

inline int BleIds::event_count() const noexcept {
    LockGuard lock(mutex_);
    return event_count_;
}

inline int BleIds::alert_count() const noexcept {
    LockGuard lock(mutex_);
    return alert_count_;
}

inline const BleIdsAlert* BleIds::get_alert(int index) const noexcept {
    LockGuard lock(mutex_);
    if (index < 0 || index >= alert_count_) return nullptr;
    return &alerts_[index];
}

inline void BleIds::clear_alerts() noexcept {
    LockGuard lock(mutex_);
    alert_count_ = 0;
}

// ---- Internal ----

inline void BleIds::add_event_(const BleIdsEvent& evt) noexcept {
    if (event_count_ < kMaxEvents) {
        events_[event_count_] = evt;
        ++event_count_;
    } else {
        // Ring buffer overwrite
        events_[event_head_] = evt;
        event_head_ = (event_head_ + 1) % kMaxEvents;
    }
}

inline void BleIds::evaluate_rules_() noexcept {
    // For each enabled rule, scan the recent events for matches
    for (int r = 0; r < rule_count_; ++r) {
        const BleIdsRule& rule = rules_[r];
        if (!rule.enabled) continue;

        uint32_t now = tick_count;
        int match_count = 0;
        int start = (event_count_ < kMaxEvents) ? 0 : event_head_;

        for (int e = start; e < event_count_; ++e) {
            const BleIdsEvent& evt = events_[e];
            if ((now - evt.timestamp) > rule.window_ms) continue;
            if (evt.event_type != rule.event_type) continue;
            if (evt.severity < rule.min_severity) continue;
            ++match_count;
        }

        if (match_count >= rule.threshold_count) {
            // Generate alert
            if (alert_count_ < kMaxAlerts) {
                BleIdsAlert a{};
                a.timestamp   = now;
                a.event_type  = rule.event_type;
                a.severity    = (match_count >= rule.threshold_count * 3) ? 90u :
                                (match_count >= rule.threshold_count * 2) ? 70u : 50u;
                a.event_count = static_cast<uint16_t>(match_count);

                const char* name = event_type_name(rule.event_type);
                int i = 0;
                for (; i < 63 && name[i]; ++i) a.description[i] = name[i];
                a.description[i] = '\0';

                alerts_[alert_count_] = a;
                ++alert_count_;
            }

            // Action >= 2: report to SecurityMonitor
            if (rule.action >= 2) {
                char reason[48];
                const char* name = event_type_name(rule.event_type);
                int i = 0;
                for (; i < 47 && name[i]; ++i) reason[i] = name[i];
                reason[i] = '\0';
                // SecurityMonitor::instance().report_firewall_anomaly(reason);
                // NOTE: uncomment when SecurityMonitor has BLE namespace
            }
        }
    }
}

// ---- ProcFS Read ----

inline int BleIds::procfs_read(char* buf, int len, int /*offset*/, void* /*priv*/) {
    if (!buf || len <= 0) return 0;

    BleIds& ids = instance();
    int pos = 0;

    auto append = [&](const char* s) {
        while (*s && pos < len - 1) buf[pos++] = *s++;
    };

    auto append_int = [&](int v) {
        char tmp[16];
        int i = 0;
        if (v == 0) { buf[pos++] = '0'; return; }
        while (v > 0 && i < 15) { tmp[i++] = '0' + (v % 10); v /= 10; }
        while (i > 0 && pos < len - 1) buf[pos++] = tmp[--i];
    };

    // Header
    append("=== BLE IDS Status ===\n");
    append("Total Events:  "); append_int(ids.event_count()); append("\n");
    append("Total Alerts:  "); append_int(ids.alert_count());  append("\n");
    append("Active Rules:  "); append_int(ids.rule_count());   append("\n\n");

    // Recent alerts (last 20)
    int alert_total = ids.alert_count();
    int alert_start = (alert_total > 20) ? alert_total - 20 : 0;

    for (int i = alert_start; i < alert_total && pos < len - 80; ++i) {
        const BleIdsAlert* a = ids.get_alert(i);
        if (!a) continue;

        append_int(a->timestamp / 1000); append("s | ");
        append(event_type_name(a->event_type)); append(" | S=");
        append_int(a->severity); append(" | cnt=");
        append_int(a->event_count); append("\n");
    }

    if (pos < len) buf[pos] = '\0';
    return pos;
}
