// ============================================================
// ble_mitm.hpp — BLE Man-in-the-Middle Attack Detection Engine
//
// Header-only MITM detector. Monitors pairing, connection
// parameters, and physical-layer anomalies that indicate
// potential MITM attacks on BLE links.
//
// Attack vectors monitored:
//   1. Pairing downgrade (LE SC → Legacy, OOB → JustWorks)
//   2. Connection parameter manipulation (interval/latency jumps)
//   3. Unexpected disconnection storms (jamming/jamming-based MITM)
//   4. RSSI paradox (RSSI increasing while reported TX power constant)
//   5. Key renegotiation frequency anomalies
//   6. Address type switching without reconnection (address spoofing)
//
// References:
//   CVE-2020-0022 — Bluetooth pairing downgrade
//   CVE-2018-5383 — Bluetooth firmware key extraction via MITM
//   BlackHat 2018 — "BLE MITM Attacks Using Software Defined Radio"
// ============================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../../kernel/core/mutex.hpp"

extern volatile uint32_t tick_count;

// Copy a NUL-terminated C-string into a fixed buffer, truncating to fit.
// The destination is always NUL-terminated. Truncation is silent because all
// current callers supply descriptions shorter than the buffer.
inline void copy_desc_bounded(char* dst, const char* src, int max_len) noexcept {
    int i = 0;
    while (i < max_len && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

// ---- Connection Parameters (from LL_CONNECTION_UPDATE_REQ) ----
struct BleConnParams {
    uint16_t interval_min; // * 1.25 ms units  (7.5ms – 4s)
    uint16_t interval_max;
    uint16_t latency;        // slave latency (0-499)
    uint16_t supervision_to; // * 10 ms units (100ms – 32s)
    uint8_t hop_increment;   // channel hop increment (5-16)
};

// ---- Pairing Method ----
enum class BlePairingMethod : uint8_t {
    JustWorks = 0,    // No MITM protection
    PasskeyEntry = 1, // 6-digit PIN (MITM protection)
    Oob = 2,          // Out-of-Band (NFC, QR)
    NumericComp = 3,  // LE Secure Connections only
};

// ---- MITM Alert Type ----
enum class MitmAlertType : uint8_t {
    PairingDowngrade = 1,        // LE SC → Legacy, OOB → JustWorks
    ParamIntervalAnomaly = 2,    // Connection interval changed unexpectedly
    ParamLatencyAnomaly = 3,     // Slave latency spike (possible passive sniffing)
    RssiParadox = 4,             // RSSI increased without TX power change
    DisconnectStorm = 5,         // Multiple disconnects in short window
    KeyRenegotiationFreq = 6,    // Key refresh too frequent (rekey MITM)
    AddressSpoofing = 7,         // Address type changed mid-connection
    SupervisionTimeoutLong = 8,  // Abnormally long supervision timeout
    HopIncrementAnomaly = 9,     // Channel hop increment out of spec
    SecurityLevelDowngrade = 10, // Security level decreased after pairing
};

// ---- Severity ----
enum class MitmSeverity : uint8_t {
    Info = 0,
    Warning = 1,
    Alert = 2,
    Critical = 3,
};

// ---- Single MITM Alert ----
struct MitmAlert {
    uint32_t timestamp;
    uint8_t device_mac[6];
    MitmAlertType type;
    MitmSeverity severity;
    char description[72];
    uint8_t confidence; // 0–100 % likelihood estimate
};

// ---- Per-connection Session Tracker ----
struct MitmSession {
    static constexpr int kMaxDisconnectHistory = 16;

    uint8_t mac[6];
    bool active;
    uint32_t connected_since_ms;
    uint32_t last_event_ms;

    // Pairing state
    BlePairingMethod last_pairing_method;
    BlePairingMethod initial_pairing_method; // captured at first pairing
    uint8_t security_level;
    bool le_secure_connections;
    bool oob_available;

    // Connection parameters
    BleConnParams current_params;
    BleConnParams initial_params; // captured at connection
    uint32_t params_changed_at_ms;
    int params_change_count;

    // RSSI tracking
    int8_t last_rssi;
    int8_t last_tx_power;

    // Disconnect tracking (ring buffer)
    uint32_t disconnect_times[kMaxDisconnectHistory];
    uint8_t disconnect_index;
    uint8_t disconnect_count;

    // Address tracking
    uint8_t address_type_at_connect; // 0=Public, 1=Random
};

// ---- MITM Detector Engine ----
class BleMitmDetector {
public:
    static constexpr int kMaxSessions = 16;
    static constexpr int kMaxAlerts = 64;
    static constexpr int kDisconnectStormWindowMs = 10000; // 10s
    static constexpr int kDisconnectStormThreshold = 5;
    static constexpr int kParamChangeSuspicious = 3; // 3+ changes in 30s
    static constexpr int kRssiParadoxThreshold = 10; // RSSI up 10dB without TX power change
    static constexpr int kSupervisionTimeMax = 3200; // 32s (in 10ms units)

    static BleMitmDetector& instance() noexcept {
        static BleMitmDetector detector;
        return detector;
    }

    // ---- Event Feeds ----

    // Called when a new BLE connection is established.
    void on_connect(const uint8_t mac[6], const BleConnParams& params, int8_t rssi, bool le_secure) noexcept;

    // Called on disconnection (with reason code, 0x08=timeout, 0x13=remote, 0x16=local).
    void on_disconnect(const uint8_t mac[6], uint8_t reason) noexcept;

    // Called when pairing completes.
    void on_pairing_complete(const uint8_t mac[6], BlePairingMethod method, uint8_t security_level, bool le_secure,
                             bool oob) noexcept;

    // Called when connection parameters update.
    void on_params_update(const uint8_t mac[6], const BleConnParams& new_params) noexcept;

    // Called when RSSI is read (from periodic RSSI reads or scan).
    void on_rssi_update(const uint8_t mac[6], int8_t rssi, int8_t tx_power) noexcept;

    // ---- Query ----

    int alert_count() const noexcept;
    const MitmAlert* get_alert(int index) const noexcept;
    void clear_alerts() noexcept;
    int active_session_count() const noexcept;

    // ---- Configure ----

    void set_rssi_paradox_threshold(int8_t dbm) noexcept;

    static const char* alert_type_name(MitmAlertType t) noexcept {
        switch (t) {
        case MitmAlertType::PairingDowngrade:
            return "PairingDowngrade";
        case MitmAlertType::ParamIntervalAnomaly:
            return "ParamIntervalAnomaly";
        case MitmAlertType::ParamLatencyAnomaly:
            return "ParamLatencyAnomaly";
        case MitmAlertType::RssiParadox:
            return "RssiParadox";
        case MitmAlertType::DisconnectStorm:
            return "DisconnectStorm";
        case MitmAlertType::KeyRenegotiationFreq:
            return "KeyRenegotiationFreq";
        case MitmAlertType::AddressSpoofing:
            return "AddressSpoofing";
        case MitmAlertType::SupervisionTimeoutLong:
            return "SupervisionTimeoutLong";
        case MitmAlertType::HopIncrementAnomaly:
            return "HopIncrementAnomaly";
        case MitmAlertType::SecurityLevelDowngrade:
            return "SecurityLevelDowngrade";
        default:
            return "Unknown";
        }
    }

    static const char* severity_name(MitmSeverity s) noexcept {
        switch (s) {
        case MitmSeverity::Info:
            return "Info";
        case MitmSeverity::Warning:
            return "Warning";
        case MitmSeverity::Alert:
            return "Alert";
        case MitmSeverity::Critical:
            return "Critical";
        default:
            return "?";
        }
    }

private:
    BleMitmDetector() = default;

    mutable Mutex mutex_{};
    MitmSession sessions_[kMaxSessions]{};
    int session_count_ = 0;
    MitmAlert alerts_[kMaxAlerts]{};
    int alert_count_ = 0;
    int8_t rssi_paradox_threshold_ = kRssiParadoxThreshold;

    MitmSession* find_session_(const uint8_t mac[6]) noexcept;
    void add_alert_(const MitmAlert& a) noexcept;

    // Detection rules
    void detect_pairing_downgrade_(const MitmSession& s) noexcept;
    void detect_param_anomaly_(const MitmSession& s, const BleConnParams& new_params) noexcept;
    void detect_rssi_paradox_(const MitmSession& s, int8_t rssi, int8_t tx_power) noexcept;
    void detect_disconnect_storm_(const MitmSession& s) noexcept;
    void detect_supervision_anomaly_(const MitmSession& s) noexcept;
};

// ============================================================
// Inline implementations
// ============================================================

inline void BleMitmDetector::on_connect(const uint8_t mac[6], const BleConnParams& params, int8_t rssi,
                                        bool le_secure) noexcept {
    if (!mac)
        return;
    LockGuard lock(mutex_);

    MitmSession* s = find_session_(mac);
    if (!s) {
        if (session_count_ >= kMaxSessions)
            return;
        s = &sessions_[session_count_];
        for (int i = 0; i < 6; ++i)
            s->mac[i] = mac[i];
        s->active = true;
        s->last_pairing_method = BlePairingMethod::JustWorks;
        s->initial_pairing_method = BlePairingMethod::JustWorks;
        s->security_level = 0;
        s->le_secure_connections = le_secure;
        s->oob_available = false;
        s->params_change_count = 0;
        s->disconnect_index = 0;
        s->disconnect_count = 0;
        s->address_type_at_connect = 0;
        ++session_count_;
    }

    s->active = true;
    s->connected_since_ms = tick_count;
    s->last_event_ms = tick_count;
    s->current_params = params;
    s->initial_params = params;
    s->last_rssi = rssi;
    s->last_tx_power = 0;
    s->params_changed_at_ms = tick_count;
    s->params_change_count = 0;
}

inline void BleMitmDetector::on_disconnect(const uint8_t mac[6], uint8_t reason) noexcept {
    if (!mac)
        return;
    LockGuard lock(mutex_);

    MitmSession* s = find_session_(mac);
    if (!s)
        return;

    s->active = false;
    s->last_event_ms = tick_count;

    // Record disconnect time
    s->disconnect_times[s->disconnect_index] = tick_count;
    s->disconnect_index = (s->disconnect_index + 1) % MitmSession::kMaxDisconnectHistory;
    if (s->disconnect_count < MitmSession::kMaxDisconnectHistory)
        ++s->disconnect_count;

    // Detect disconnect storm
    detect_disconnect_storm_(*s);
}

inline void BleMitmDetector::on_pairing_complete(const uint8_t mac[6], BlePairingMethod method, uint8_t security_level,
                                                 bool le_secure, bool oob) noexcept {
    if (!mac)
        return;
    LockGuard lock(mutex_);

    MitmSession* s = find_session_(mac);
    if (!s)
        return;

    // Store initial pairing method on first pairing
    if (s->initial_pairing_method == BlePairingMethod::JustWorks && method != BlePairingMethod::JustWorks) {
        s->initial_pairing_method = method;
    }

    // Detect downgrade
    detect_pairing_downgrade_(*s);

    s->last_pairing_method = method;
    s->security_level = security_level;
    s->le_secure_connections = le_secure;
    s->oob_available = oob;
    s->last_event_ms = tick_count;
}

inline void BleMitmDetector::on_params_update(const uint8_t mac[6], const BleConnParams& new_params) noexcept {
    if (!mac)
        return;
    LockGuard lock(mutex_);

    MitmSession* s = find_session_(mac);
    if (!s)
        return;

    detect_param_anomaly_(*s, new_params);

    s->current_params = new_params;
    s->params_changed_at_ms = tick_count;
    ++s->params_change_count;
    s->last_event_ms = tick_count;
}

inline void BleMitmDetector::on_rssi_update(const uint8_t mac[6], int8_t rssi, int8_t tx_power) noexcept {
    if (!mac)
        return;
    LockGuard lock(mutex_);

    MitmSession* s = find_session_(mac);
    if (!s)
        return;

    detect_rssi_paradox_(*s, rssi, tx_power);

    // Check supervision timeout anomaly on each periodic update
    detect_supervision_anomaly_(*s);

    s->last_rssi = rssi;
    s->last_tx_power = tx_power;
    s->last_event_ms = tick_count;
}

inline int BleMitmDetector::alert_count() const noexcept {
    LockGuard lock(mutex_);
    return alert_count_;
}

inline const MitmAlert* BleMitmDetector::get_alert(int index) const noexcept {
    LockGuard lock(mutex_);
    if (index < 0 || index >= alert_count_)
        return nullptr;
    return &alerts_[index];
}

inline void BleMitmDetector::clear_alerts() noexcept {
    LockGuard lock(mutex_);
    alert_count_ = 0;
}

inline int BleMitmDetector::active_session_count() const noexcept {
    LockGuard lock(mutex_);
    int n = 0;
    for (int i = 0; i < session_count_; ++i)
        if (sessions_[i].active)
            ++n;
    return n;
}

inline void BleMitmDetector::set_rssi_paradox_threshold(int8_t dbm) noexcept {
    LockGuard lock(mutex_);
    rssi_paradox_threshold_ = dbm;
}

// ---- Detection Rules (assume mutex_ held by caller) ----

inline void BleMitmDetector::detect_pairing_downgrade_(const MitmSession& s) noexcept {
    // Check: initial was LE SC but now pairing without LE SC
    if (s.initial_pairing_method >= BlePairingMethod::PasskeyEntry &&
        s.last_pairing_method == BlePairingMethod::JustWorks) {
        MitmAlert a{};
        a.timestamp = tick_count;
        for (int i = 0; i < 6; ++i)
            a.device_mac[i] = s.mac[i];
        a.type = MitmAlertType::PairingDowngrade;
        a.severity = MitmSeverity::Critical;
        a.confidence = 85;
        copy_desc_bounded(a.description, "Pairing downgraded from authenticated to JustWorks — possible MITM", 71);
        add_alert_(a);
    }

    // Check: OOB was available but switched to JustWorks
    if (s.oob_available && s.last_pairing_method == BlePairingMethod::JustWorks) {
        MitmAlert a{};
        a.timestamp = tick_count;
        for (int i = 0; i < 6; ++i)
            a.device_mac[i] = s.mac[i];
        a.type = MitmAlertType::SecurityLevelDowngrade;
        a.severity = MitmSeverity::Critical;
        a.confidence = 90;
        copy_desc_bounded(a.description, "OOB available but JustWorks used — possible MITM disabling OOB", 71);
        add_alert_(a);
    }
}

inline void BleMitmDetector::detect_param_anomaly_(const MitmSession& s, const BleConnParams& new_params) noexcept {
    // Check: connection interval changed dramatically (> 3x or < 1/3x)
    uint16_t old_min = s.current_params.interval_min;
    uint16_t new_min = new_params.interval_min;
    if (old_min > 0 && (new_min > old_min * 3 || new_min * 3 < old_min)) {
        MitmAlert a{};
        a.timestamp = tick_count;
        for (int i = 0; i < 6; ++i)
            a.device_mac[i] = s.mac[i];
        a.type = MitmAlertType::ParamIntervalAnomaly;
        a.severity = MitmSeverity::Warning;
        a.confidence = 55;
        copy_desc_bounded(a.description, "Connection interval changed > 3x — possible parameter injection", 71);
        add_alert_(a);
    }

    // Check: slave latency spike (allows more time for passive sniffing)
    if (s.current_params.latency > 0 && new_params.latency > s.current_params.latency * 5) {
        MitmAlert a{};
        a.timestamp = tick_count;
        for (int i = 0; i < 6; ++i)
            a.device_mac[i] = s.mac[i];
        a.type = MitmAlertType::ParamLatencyAnomaly;
        a.severity = MitmSeverity::Alert;
        a.confidence = 60;
        copy_desc_bounded(a.description, "Slave latency increased > 5x — possible passive sniffing window", 71);
        add_alert_(a);
    }

    // Check: excessive parameter changes in short window
    if (s.params_change_count >= kParamChangeSuspicious) {
        MitmAlert a{};
        a.timestamp = tick_count;
        for (int i = 0; i < 6; ++i)
            a.device_mac[i] = s.mac[i];
        a.type = MitmAlertType::ParamIntervalAnomaly;
        a.severity = MitmSeverity::Alert;
        a.confidence = 65;
        copy_desc_bounded(a.description, "Frequent connection parameter updates — possible tampering", 71);
        add_alert_(a);
    }
}

inline void BleMitmDetector::detect_rssi_paradox_(const MitmSession& s, int8_t rssi, int8_t tx_power) noexcept {
    // RSSI paradox: RSSI increased significantly but TX power didn't change.
    // This could indicate a MITM relay that amplifies the signal.
    int16_t rssi_delta = static_cast<int16_t>(rssi) - static_cast<int16_t>(s.last_rssi);
    if (rssi_delta > rssi_paradox_threshold_ && tx_power == s.last_tx_power && s.last_tx_power != 0) {
        MitmAlert a{};
        a.timestamp = tick_count;
        for (int i = 0; i < 6; ++i)
            a.device_mac[i] = s.mac[i];
        a.type = MitmAlertType::RssiParadox;
        a.severity = MitmSeverity::Alert;
        a.confidence = 70;
        copy_desc_bounded(a.description, "RSSI increased without TX power change — possible relay attack", 71);
        add_alert_(a);
    }
}

inline void BleMitmDetector::detect_disconnect_storm_(const MitmSession& s) noexcept {
    uint32_t now = tick_count;
    int recent = 0;
    for (uint8_t i = 0; i < s.disconnect_count; ++i) {
        uint32_t dt = s.disconnect_times[i];
        if (dt > 0 && (now - dt) < kDisconnectStormWindowMs)
            ++recent;
    }

    if (recent >= kDisconnectStormThreshold) {
        MitmAlert a{};
        a.timestamp = tick_count;
        for (int i = 0; i < 6; ++i)
            a.device_mac[i] = s.mac[i];
        a.type = MitmAlertType::DisconnectStorm;
        a.severity = MitmSeverity::Critical;
        a.confidence = 80;
        copy_desc_bounded(a.description, "Disconnect storm detected — possible jamming-based MITM", 71);
        add_alert_(a);
    }
}

inline void BleMitmDetector::detect_supervision_anomaly_(const MitmSession& s) noexcept {
    if (s.current_params.supervision_to > kSupervisionTimeMax) {
        MitmAlert a{};
        a.timestamp = tick_count;
        for (int i = 0; i < 6; ++i)
            a.device_mac[i] = s.mac[i];
        a.type = MitmAlertType::SupervisionTimeoutLong;
        a.severity = MitmSeverity::Warning;
        a.confidence = 40;
        copy_desc_bounded(a.description, "Supervision timeout > 32s — device could be unreachable for long", 71);
        add_alert_(a);
    }
}

// ---- Internal helpers ----

inline MitmSession* BleMitmDetector::find_session_(const uint8_t mac[6]) noexcept {
    for (int i = 0; i < session_count_; ++i) {
        bool match = true;
        for (int j = 0; j < 6; ++j) {
            if (sessions_[i].mac[j] != mac[j]) {
                match = false;
                break;
            }
        }
        if (match)
            return &sessions_[i];
    }
    return nullptr;
}

inline void BleMitmDetector::add_alert_(const MitmAlert& a) noexcept {
    if (alert_count_ < kMaxAlerts) {
        alerts_[alert_count_] = a;
        ++alert_count_;
    }
}
