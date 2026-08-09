// ============================================================
// gatt_auditor.hpp — GATT Service Security Auditor
//
// Header-only GATT security analysis. Audits characteristics
// (permissions, encryption requirements, authentication) and
// produces severity-graded findings.  Integrated with
// SecurityMonitor for critical issues.
//
// References:
//   Bluetooth Core Spec v5.3, Vol 3 Part G (GATT)
//   BLE Security Modes: Mode 1 (encryption), Mode 2 (signing)
//   Levels: 1=No Security, 2=Unauth Encrypt, 3=Auth Encrypt, 4=LE SC
// ============================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../../kernel/mutex.hpp"

extern volatile uint32_t tick_count;

// ---- GATT Characteristic Property flags (Vol 3 Part G §3.3.1.1) ----
namespace GattProp {
    constexpr uint8_t Broadcast   = 0x01;
    constexpr uint8_t Read        = 0x02;
    constexpr uint8_t WriteNoResp = 0x04;
    constexpr uint8_t Write       = 0x08;
    constexpr uint8_t Notify      = 0x10;
    constexpr uint8_t Indicate    = 0x12;
    constexpr uint8_t AuthSigned  = 0x40;
    constexpr uint8_t Extended    = 0x80;
}

// ---- Audit Severity ----
enum class GattAuditSeverity : uint8_t {
    None     = 0,
    Info     = 1,
    Warning  = 2,
    Critical = 3,
};

// ---- Auditor Finding Categories ----
enum class GattFindingType : uint8_t {
    OpenWriteNoAuth        = 1,   // Write/WriteNoResp with no encryption/authentication
    OpenReadSensitive      = 2,   // Read exposes sensitive data without encryption
    MissingEncryption      = 3,   // Characteristic requires no encryption
    WeakAuthentication     = 4,   // Mode 1 Level 2 but should be Level 3+
    JustWorksPairing       = 5,   // Device supports JustWorks (no MITM protection)
    NoOobSupport           = 6,   // No OOB pairing available
    KnownInsecureService   = 7,   // Well-known service with CVEs (e.g. ANCS bypass)
    UnauthenticatedWrite   = 8,   // Write without authentication on paired service
    InsufficientKeySize    = 9,   // Encryption key < 128 bits
};

// ---- Single GATT Characteristic Descriptor (audited) ----
struct GattCharInfo {
    static constexpr int kMaxUserDescLen = 24;

    uint16_t  handle;               // Attribute handle
    uint16_t  uuid16;               // 16-bit UUID (0 if 128-bit only)
    uint8_t   properties;           // Bitmask of GattProp flags
    uint8_t   security_level;       // 0=NoSec, 1=EncUnauth, 2=EncAuth, 3=LE_SC
    bool      requires_auth;        // Characteristic requires authentication
    bool      requires_encryption;  // Characteristic requires encryption
    char      user_description[kMaxUserDescLen + 1];
};

// ---- Single Audit Finding ----
struct GattFinding {
    uint32_t          timestamp;
    uint8_t           device_mac[6];
    uint16_t          char_handle;
    uint16_t          char_uuid16;
    GattFindingType   type;
    GattAuditSeverity severity;
    char              description[64];  // Human-readable
};

// ---- Known-insecure service UUIDs ----
namespace KnownInsecureServices {
    // ANCS (Apple Notification Center Service) — known auth bypass CVEs
    constexpr uint16_t ANCS              = 0x7905;
    // Immediate Alert — writeable, can be abused for DoS
    constexpr uint16_t ImmediateAlert    = 0x1802;
    // Link Loss — writeable alert level, potential persistent notification spam
    constexpr uint16_t LinkLoss          = 0x1803;
    // TX Power — readable, may leak indoor location info
    constexpr uint16_t TxPower           = 0x1804;
    // Battery Service — usually harmless but may expose device info
    constexpr uint16_t BatteryService    = 0x180F;
    // Device Information — exposes manufacturer/serial, PII leakage
    constexpr uint16_t DeviceInfo        = 0x180A;
    // Current Time — writeable on some devices, can be used to spoof time
    constexpr uint16_t CurrentTime       = 0x1805;
    // HID Service — keyboard/mouse input, potential keystroke injection
    constexpr uint16_t HumanInterfaceDev = 0x1812;

    // Known CVEs with BLE services
    // CVE-2023-XXXX — ANCS auth bypass
    // CVE-2020-0022 — BLE pairing downgrade
    // CVE-2018-5383 — Bluetooth firmware key extraction

    static bool is_insecure(uint16_t uuid16) noexcept {
        return uuid16 == ANCS
            || uuid16 == ImmediateAlert
            || uuid16 == LinkLoss
            || uuid16 == CurrentTime
            || uuid16 == HumanInterfaceDev;
    }

    static const char* description(uint16_t uuid16) noexcept {
        switch (uuid16) {
            case ANCS:               return "ANCS: known auth bypass CVEs";
            case ImmediateAlert:     return "Immediate Alert: DoS via write flooding";
            case LinkLoss:           return "Link Loss: persistent notification spam";
            case CurrentTime:        return "Current Time: spoofable via write without auth";
            case HumanInterfaceDev:  return "HID: potential keystroke injection";
            default:                 return "";
        }
    }
}

// ---- GATT Auditor Engine ----
class GattAuditor {
public:
    static constexpr int kMaxFindings      = 64;
    static constexpr int kMaxCharsPerDev   = 32;
    static constexpr int kMaxAuditedDevices = 8;

    struct AuditedDevice {
        uint8_t     mac[6];
        bool        connected;
        uint32_t    connected_since_ms;
        GattCharInfo characteristics[kMaxCharsPerDev];
        uint8_t     char_count;
        bool        just_works_supported;     // from advertising/flags
        bool        le_secure_connections;    // from advertising
    };

    static GattAuditor& instance() noexcept {
        static GattAuditor auditor;
        return auditor;
    }

    // ---- Device Tracking ----

    // Register a device for GATT auditing (after connection).
    AuditedDevice* register_device(const uint8_t mac[6]) noexcept;

    // Unregister on disconnect.
    void unregister_device(const uint8_t mac[6]) noexcept;

    // Add a discovered characteristic for a device.
    bool add_characteristic(const uint8_t mac[6], const GattCharInfo& ch) noexcept;

    // ---- Security Analysis ----

    // Run full audit on all registered devices.
    // Returns number of new findings produced.
    int audit_all() noexcept;

    // Audit a single device.
    int audit_device(const uint8_t mac[6]) noexcept;

    // ---- Query ----

    int finding_count() const noexcept;
    const GattFinding* get_finding(int index) const noexcept;
    void clear_findings() noexcept;

    // ---- Device Characteristic Query ----

    int audited_device_count() const noexcept;
    const AuditedDevice* get_audited_device(int index) const noexcept;

    // ---- PA / Security Mode Helper ----

    // Map BLE Security Mode + Level to our internal level
    static uint8_t map_security_level(uint8_t mode, uint8_t level) noexcept {
        // Mode 1 (Encryption): Level 1=NoSec, 2=UnauthEnc, 3=AuthEnc, 4=LESC
        // Mode 2 (Signing):    Level 1=NoSec, 2=UnauthSign
        if (mode == 1) {
            return (level > 0 && level <= 4) ? level : 0;
        }
        if (mode == 2) {
            return (level >= 2) ? 2 : 0;  // signing maps to level 2
        }
        return 0;
    }

    static const char* severity_name(GattAuditSeverity s) noexcept {
        switch (s) {
            case GattAuditSeverity::None:     return "None";
            case GattAuditSeverity::Info:     return "Info";
            case GattAuditSeverity::Warning:  return "Warning";
            case GattAuditSeverity::Critical: return "Critical";
            default:                          return "?";
        }
    }

    static const char* finding_type_name(GattFindingType t) noexcept {
        switch (t) {
            case GattFindingType::OpenWriteNoAuth:      return "OpenWriteNoAuth";
            case GattFindingType::OpenReadSensitive:    return "OpenReadSensitive";
            case GattFindingType::MissingEncryption:    return "MissingEncryption";
            case GattFindingType::WeakAuthentication:   return "WeakAuthentication";
            case GattFindingType::JustWorksPairing:     return "JustWorksPairing";
            case GattFindingType::NoOobSupport:         return "NoOobSupport";
            case GattFindingType::KnownInsecureService: return "KnownInsecureService";
            case GattFindingType::UnauthenticatedWrite: return "UnauthenticatedWrite";
            case GattFindingType::InsufficientKeySize:  return "InsufficientKeySize";
            default:                                    return "Unknown";
        }
    }

private:
    GattAuditor() = default;

    mutable Mutex mutex_;
    AuditedDevice audited_devices_[kMaxAuditedDevices];
    uint8_t       audited_count_ = 0;
    GattFinding   findings_[kMaxFindings];
    int           finding_count_ = 0;

    AuditedDevice* find_device_(const uint8_t mac[6]) noexcept;
    void add_finding_(GattFinding f) noexcept;
    void audit_char_(const AuditedDevice& dev, const GattCharInfo& ch) noexcept;
};

// ============================================================
// Inline implementations
// ============================================================

inline GattAuditor::AuditedDevice* GattAuditor::register_device(
    const uint8_t mac[6]) noexcept
{
    if (!mac) return nullptr;
    LockGuard lock(mutex_);

    // Reuse existing slot if already registered
    AuditedDevice* existing = find_device_(mac);
    if (existing) {
        existing->connected = true;
        existing->connected_since_ms = tick_count;
        return existing;
    }

    if (audited_count_ >= kMaxAuditedDevices) return nullptr;

    AuditedDevice* dev = &audited_devices_[audited_count_];
    for (int i = 0; i < 6; ++i) dev->mac[i] = mac[i];
    dev->connected              = true;
    dev->connected_since_ms     = tick_count;
    dev->char_count             = 0;
    dev->just_works_supported   = false;
    dev->le_secure_connections  = false;
    ++audited_count_;
    return dev;
}

inline void GattAuditor::unregister_device(const uint8_t mac[6]) noexcept {
    if (!mac) return;
    LockGuard lock(mutex_);
    AuditedDevice* dev = find_device_(mac);
    if (dev) dev->connected = false;
}

inline bool GattAuditor::add_characteristic(
    const uint8_t mac[6], const GattCharInfo& ch) noexcept
{
    if (!mac) return false;
    LockGuard lock(mutex_);
    AuditedDevice* dev = find_device_(mac);
    if (!dev || dev->char_count >= kMaxCharsPerDev) return false;

    dev->characteristics[dev->char_count] = ch;
    ++dev->char_count;
    return true;
}

inline int GattAuditor::audit_all() noexcept {
    LockGuard lock(mutex_);
    int total_new = 0;
    for (uint8_t i = 0; i < audited_count_; ++i) {
        if (!audited_devices_[i].connected) continue;
        total_new += audit_device(audited_devices_[i].mac);
    }
    return total_new;
}

inline int GattAuditor::audit_device(const uint8_t mac[6]) noexcept {
    // NOTE: callers hold mutex_
    AuditedDevice* dev = find_device_(mac);
    if (!dev || !dev->connected) return 0;

    int old_count = finding_count_;

    // ---- Check 1: JustWorks pairing ----
    if (dev->just_works_supported && !dev->le_secure_connections) {
        GattFinding f{};
        f.timestamp    = tick_count;
        for (int i = 0; i < 6; ++i) f.device_mac[i] = dev->mac[i];
        f.char_handle  = 0;
        f.char_uuid16  = 0;
        f.type         = GattFindingType::JustWorksPairing;
        f.severity     = GattAuditSeverity::Warning;
        const char* desc = "Device uses JustWorks pairing — no MITM protection";
        for (int i = 0; i < 63 && desc[i]; ++i) f.description[i] = desc[i];
        f.description[63] = '\0';
        add_finding_(f);
    }

    // ---- Check 2: No OOB support ----
    if (!dev->le_secure_connections) {
        GattFinding f{};
        f.timestamp    = tick_count;
        for (int i = 0; i < 6; ++i) f.device_mac[i] = dev->mac[i];
        f.char_handle  = 0;
        f.char_uuid16  = 0;
        f.type         = GattFindingType::NoOobSupport;
        f.severity     = GattAuditSeverity::Info;
        const char* desc = "No OOB pairing support detected";
        for (int i = 0; i < 63 && desc[i]; ++i) f.description[i] = desc[i];
        f.description[63] = '\0';
        add_finding_(f);
    }

    // ---- Check 3: Per-characteristic audits ----
    for (uint8_t i = 0; i < dev->char_count; ++i) {
        audit_char_(*dev, dev->characteristics[i]);
    }

    return finding_count_ - old_count;
}

inline void GattAuditor::audit_char_(
    const AuditedDevice& dev, const GattCharInfo& ch) noexcept
{
    GattFinding f{};
    f.timestamp   = tick_count;
    for (int i = 0; i < 6; ++i) f.device_mac[i] = dev.mac[i];
    f.char_handle = ch.handle;
    f.char_uuid16 = ch.uuid16;

    bool is_writable = (ch.properties & (GattProp::Write | GattProp::WriteNoResp)) != 0;
    bool is_readable = (ch.properties & GattProp::Read) != 0;

    // Check: Write without authentication
    if (is_writable && ch.security_level < 2) {
        f.type     = GattFindingType::OpenWriteNoAuth;
        f.severity = (ch.security_level == 0) ? GattAuditSeverity::Critical
                                                : GattAuditSeverity::Warning;
        const char* desc = "Writeable characteristic without encryption";
        for (int i = 0; i < 63 && desc[i]; ++i) f.description[i] = desc[i];
        f.description[63] = '\0';
        add_finding_(f);
    }

    // Check: Missing encryption on readable sensitive data
    if (is_readable && ch.security_level == 0) {
        f.type     = GattFindingType::MissingEncryption;
        f.severity = GattAuditSeverity::Warning;
        const char* desc = "Readable characteristic without encryption";
        for (int i = 0; i < 63 && desc[i]; ++i) f.description[i] = desc[i];
        f.description[63] = '\0';
        add_finding_(f);
    }

    // Check: Weak authentication (Mode 1 Level 2 → should be Level 3+)
    if (is_writable && ch.security_level == 2 && !ch.requires_auth) {
        f.type     = GattFindingType::WeakAuthentication;
        f.severity = GattAuditSeverity::Warning;
        const char* desc = "Encryption without authentication — MITM possible";
        for (int i = 0; i < 63 && desc[i]; ++i) f.description[i] = desc[i];
        f.description[63] = '\0';
        add_finding_(f);
    }

    // Check: Known-insecure service UUID
    if (ch.uuid16 != 0 && KnownInsecureServices::is_insecure(ch.uuid16)) {
        f.type     = GattFindingType::KnownInsecureService;
        f.severity = GattAuditSeverity::Critical;
        const char* svc_desc = KnownInsecureServices::description(ch.uuid16);
        for (int i = 0; i < 63 && svc_desc[i]; ++i) f.description[i] = svc_desc[i];
        f.description[63] = '\0';
        add_finding_(f);
    }
}

inline int GattAuditor::finding_count() const noexcept {
    LockGuard lock(mutex_);
    return finding_count_;
}

inline const GattFinding* GattAuditor::get_finding(int index) const noexcept {
    LockGuard lock(mutex_);
    if (index < 0 || index >= finding_count_) return nullptr;
    return &findings_[index];
}

inline void GattAuditor::clear_findings() noexcept {
    LockGuard lock(mutex_);
    finding_count_ = 0;
}

inline int GattAuditor::audited_device_count() const noexcept {
    LockGuard lock(mutex_);
    return audited_count_;
}

inline const GattAuditor::AuditedDevice* GattAuditor::get_audited_device(
    int index) const noexcept
{
    LockGuard lock(mutex_);
    if (index < 0 || index >= audited_count_) return nullptr;
    return &audited_devices_[index];
}

// ---- Internal ----

inline GattAuditor::AuditedDevice* GattAuditor::find_device_(
    const uint8_t mac[6]) noexcept
{
    for (uint8_t i = 0; i < audited_count_; ++i) {
        bool match = true;
        for (int j = 0; j < 6; ++j) {
            if (audited_devices_[i].mac[j] != mac[j]) { match = false; break; }
        }
        if (match) return &audited_devices_[i];
    }
    return nullptr;
}

inline void GattAuditor::add_finding_(GattFinding f) noexcept {
    if (finding_count_ < kMaxFindings) {
        findings_[finding_count_] = f;
        ++finding_count_;
    }
}
