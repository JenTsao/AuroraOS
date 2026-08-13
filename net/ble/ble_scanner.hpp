// ============================================================
// ble_scanner.hpp — BLE Device Discovery & Fingerprinting Engine
//
// Header-only BLE passive scanner. Parses advertising packets
// (AD structures), builds a device fingerprint database, and
// tracks RSSI over time for proximity/location analysis.
//
// References:
//   Bluetooth Core Spec v5.3, Vol 3 Part C §11 (AD types)
//   Bluetooth Core Spec v5.3, Vol 6 Part B §2.3 (address types)
//
// Dependencies:
//   kernel/mutex.hpp   — Mutex, LockGuard
//   net/ble/hal_ble.hpp — HalBle abstraction (scan control)
//   extern tick_count   — SysTick wall clock
// ============================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../../kernel/core/mutex.hpp"

// ---- Forward: SysTick millisecond counter ----
extern volatile uint32_t tick_count;

// ---- AD Type Constants (Bluetooth Assigned Numbers) ----
namespace BleAdType {
constexpr uint8_t Flags = 0x01;
constexpr uint8_t IncompleteUuid16 = 0x02;
constexpr uint8_t CompleteUuid16 = 0x03;
constexpr uint8_t IncompleteUuid32 = 0x04;
constexpr uint8_t CompleteUuid32 = 0x05;
constexpr uint8_t IncompleteUuid128 = 0x06;
constexpr uint8_t CompleteUuid128 = 0x07;
constexpr uint8_t ShortName = 0x08;
constexpr uint8_t CompleteName = 0x09;
constexpr uint8_t TxPowerLevel = 0x0A;
constexpr uint8_t ClassOfDevice = 0x0D;
constexpr uint8_t SimplePairingHashC192 = 0x0E;
constexpr uint8_t SimplePairingRandomizer = 0x0F;
constexpr uint8_t SecurityManagerTk = 0x10;
constexpr uint8_t SecurityManagerOobFlags = 0x11;
constexpr uint8_t SlaveConnectionInterval = 0x12;
constexpr uint8_t ServiceSolicitUuid16 = 0x14;
constexpr uint8_t ServiceSolicitUuid128 = 0x15;
constexpr uint8_t ServiceDataUuid16 = 0x16;
constexpr uint8_t PublicTargetAddr = 0x17;
constexpr uint8_t RandomTargetAddr = 0x18;
constexpr uint8_t Appearance = 0x19;
constexpr uint8_t AdvertisingInterval = 0x1A;
constexpr uint8_t LeDeviceAddr = 0x1B;
constexpr uint8_t LeRole = 0x1C;
constexpr uint8_t SimplePairingHashC256 = 0x1D;
constexpr uint8_t SimplePairingRandomC256 = 0x1E;
constexpr uint8_t ServiceSolicitUuid32 = 0x1F;
constexpr uint8_t ServiceDataUuid32 = 0x20;
constexpr uint8_t ServiceDataUuid128 = 0x21;
constexpr uint8_t LeSecureConfirm = 0x22;
constexpr uint8_t LeSecureRandom = 0x23;
constexpr uint8_t Uri = 0x24;
constexpr uint8_t IndoorPositioning = 0x25;
constexpr uint8_t TransportDiscoveryData = 0x26;
constexpr uint8_t LeSupportedFeatures = 0x27;
constexpr uint8_t ChannelMapUpdate = 0x28;
constexpr uint8_t PbAdv = 0x29;
constexpr uint8_t MeshMessage = 0x2A;
constexpr uint8_t MeshBeacon = 0x2B;
constexpr uint8_t BigInfo = 0x2C;
constexpr uint8_t BroadcastCode = 0x2D;
constexpr uint8_t _3dInfoData = 0x3D;
constexpr uint8_t ManufacturerData = 0xFF;
} // namespace BleAdType

// ---- Advertising Flags bits ----
namespace BleAdFlag {
constexpr uint8_t LimitedDiscoverable = 0x01;
constexpr uint8_t GeneralDiscoverable = 0x02;
constexpr uint8_t BrEdrNotSupported = 0x04;
constexpr uint8_t SimulLeBrEdrCtrl = 0x08;
constexpr uint8_t SimulLeBrEdrHost = 0x10;
} // namespace BleAdFlag

// ---- Appearance category constants ----
namespace BleAppearance {
constexpr uint16_t GenericPhone = 0x0040;
constexpr uint16_t GenericWatch = 0x00C0;
constexpr uint16_t GenericComputer = 0x0080;
constexpr uint16_t GenericMediaDevice = 0x0088;
constexpr uint16_t GenericTag = 0x0200;
constexpr uint16_t GenericGlasses = 0x0320;
constexpr uint16_t GenericRing = 0x0380;
} // namespace BleAppearance

// ---- BLE Address Types ----
enum class BleAddrType : uint8_t {
    Public = 0x00,
    RandomStatic = 0x01,
    RandomPrivateResolvable = 0x02,
    RandomPrivateNonResolvable = 0x03,
    Unknown = 0xFF,
};

// ---- Device Classification ----
enum class BleDeviceClass : uint8_t {
    Unknown = 0,
    Phone = 1,
    Watch = 2,
    Headset = 3,
    Tracker = 4,
    Beacon = 5,
    Computer = 6,
    MedicalDevice = 7,
    IotSensor = 8,
    Keyboard = 9,
    Mouse = 10,
    Gamepad = 11,
};

// ---- Parsed AD Structure Entry ----
struct AdEntry {
    uint8_t type;
    uint8_t length;
    const uint8_t* data;
};

// ---- Single Device Fingerprint ----
struct BleDeviceFingerprint {
    static constexpr int kMaxRssiSamples = 16;
    static constexpr int kMaxNameLen = 28;
    static constexpr int kMaxManufacturerData = 8;

    uint8_t mac[6]; // BD_ADDR
    BleAddrType addr_type;
    BleDeviceClass device_class;

    // Advertising flags
    uint8_t ad_flags;
    bool is_connectable;
    bool is_scannable;

    // RSSI tracking (ring buffer)
    int8_t rssi_history[kMaxRssiSamples];
    uint8_t rssi_index;
    uint8_t rssi_count;
    int8_t rssi_avg;
    int8_t rssi_max;
    int8_t rssi_min;

    // Parsed advertising fields
    char name[kMaxNameLen + 1];
    uint8_t name_len;
    uint16_t appearance;
    int8_t tx_power_dbm;

    // Service UUIDs found in advertising
    uint16_t uuid16_list[4];
    uint8_t uuid16_count;

    // Manufacturer specific data (first bytes)
    uint16_t company_id;
    uint8_t manuf_data[kMaxManufacturerData];
    uint8_t manuf_data_len;

    // Timing
    uint32_t first_seen_ms;
    uint32_t last_seen_ms;
    uint32_t last_rssi_update_ms;

    // Security indicators (from advertising)
    bool has_sc_confirm;     // LE Secure Connections Confirm
    bool has_sc_random;      // LE Secure Connections Random
    bool has_oob_support;    // OOB flags present
    bool has_simple_pairing; // Simple Pairing hash present

    bool is_connecting() const noexcept {
        return is_connectable;
    }

    bool has_name() const noexcept {
        return name_len > 0;
    }

    bool is_stale(uint32_t timeout_ms) const noexcept {
        return (tick_count - last_seen_ms) > timeout_ms;
    }
};

// ---- Advertiser Scanner Engine ----
class BleScanner {
public:
    static constexpr int kMaxDevices = 64;
    static constexpr int kStaleTimeoutMs = 30000; // 30s without update → stale
    static constexpr int kAdPayloadMax = 31;      // BLE 4.x max payload

    static BleScanner& instance() noexcept {
        static BleScanner scanner;
        return scanner;
    }

    // ---- Scan Control ----

    // Feed a received advertising packet (from HalBle/HCI layer).
    // `ad_data` points to the full advertising payload (up to 31 bytes).
    // `len` is the length (1-31). `rssi` is in dBm, `addr_type` per spec.
    //
    // Returns: pointer to the updated (or newly allocated) fingerprint,
    //          or nullptr on parse error.
    const BleDeviceFingerprint* process_advertisement(const uint8_t mac[6], BleAddrType addr_type, int8_t rssi,
                                                      const uint8_t* ad_data, uint8_t len) noexcept;

    // Flush stale devices (last_seen > kStaleTimeoutMs).
    // Returns number of devices removed.
    int flush_stale() noexcept;

    // ---- Query ----
    int device_count() const noexcept;

    const BleDeviceFingerprint* find_by_mac(const uint8_t mac[6]) const noexcept;
    const BleDeviceFingerprint* get_device(int index) const noexcept;
    const BleDeviceFingerprint* find_by_name(const char* name) const noexcept;

    // Count devices by class
    int count_by_class(BleDeviceClass cls) const noexcept;

    // Find devices with weak security indicators (no SC, no OOB, JustWorks)
    int count_weak_security() const noexcept;

private:
    BleScanner() = default;

    mutable Mutex mutex_;
    BleDeviceFingerprint devices_[kMaxDevices];
    int device_count_ = 0;

    // Internal
    BleDeviceFingerprint* find_or_add_(const uint8_t mac[6]) noexcept;
    void parse_ad_structures_(BleDeviceFingerprint* dev, const uint8_t* data, uint8_t len) noexcept;
    void update_rssi_(BleDeviceFingerprint* dev, int8_t rssi) noexcept;
    void classify_device_(BleDeviceFingerprint* dev) noexcept;

    static void iter_ad_structures_(const uint8_t* data, uint8_t len, void (*cb)(const AdEntry&, void*),
                                    void* user) noexcept;
};

// ============================================================
// Inline implementations
// ============================================================

inline const BleDeviceFingerprint* BleScanner::process_advertisement(const uint8_t mac[6], BleAddrType addr_type,
                                                                     int8_t rssi, const uint8_t* ad_data,
                                                                     uint8_t len) noexcept {
    if (!mac || !ad_data || len == 0 || len > kAdPayloadMax)
        return nullptr;

    LockGuard lock(mutex_);

    BleDeviceFingerprint* dev = find_or_add_(mac);
    if (!dev)
        return nullptr;

    dev->addr_type = addr_type;
    dev->last_seen_ms = tick_count;

    // Parse AD structures
    parse_ad_structures_(dev, ad_data, len);

    // Update RSSI
    update_rssi_(dev, rssi);

    // Re-classify
    classify_device_(dev);

    return dev;
}

inline int BleScanner::flush_stale() noexcept {
    LockGuard lock(mutex_);
    int removed = 0;
    int w = 0;
    for (int r = 0; r < device_count_; ++r) {
        if (devices_[r].is_stale(kStaleTimeoutMs)) {
            ++removed;
        } else {
            if (w != r)
                devices_[w] = devices_[r];
            ++w;
        }
    }
    device_count_ = w;
    return removed;
}

inline int BleScanner::device_count() const noexcept {
    LockGuard lock(mutex_);
    return device_count_;
}

inline const BleDeviceFingerprint* BleScanner::find_by_mac(const uint8_t mac[6]) const noexcept {
    if (!mac)
        return nullptr;
    LockGuard lock(mutex_);
    for (int i = 0; i < device_count_; ++i) {
        bool match = true;
        for (int j = 0; j < 6; ++j) {
            if (devices_[i].mac[j] != mac[j]) {
                match = false;
                break;
            }
        }
        if (match)
            return &devices_[i];
    }
    return nullptr;
}

inline const BleDeviceFingerprint* BleScanner::get_device(int index) const noexcept {
    LockGuard lock(mutex_);
    if (index < 0 || index >= device_count_)
        return nullptr;
    return &devices_[index];
}

inline const BleDeviceFingerprint* BleScanner::find_by_name(const char* name) const noexcept {
    if (!name)
        return nullptr;
    LockGuard lock(mutex_);
    for (int i = 0; i < device_count_; ++i) {
        if (devices_[i].has_name() && strncmp(devices_[i].name, name, sizeof(devices_[i].name)) == 0) {
            return &devices_[i];
        }
    }
    return nullptr;
}

inline int BleScanner::count_by_class(BleDeviceClass cls) const noexcept {
    LockGuard lock(mutex_);
    int n = 0;
    for (int i = 0; i < device_count_; ++i) {
        if (devices_[i].device_class == cls)
            ++n;
    }
    return n;
}

inline int BleScanner::count_weak_security() const noexcept {
    LockGuard lock(mutex_);
    int n = 0;
    for (int i = 0; i < device_count_; ++i) {
        // Weak = no LE Secure Connections indicators in advertising
        if (!devices_[i].has_sc_confirm && !devices_[i].has_sc_random && !devices_[i].has_oob_support) {
            ++n;
        }
    }
    return n;
}

// ---- Internal helpers ----

inline BleDeviceFingerprint* BleScanner::find_or_add_(const uint8_t mac[6]) noexcept {
    // Search existing
    for (int i = 0; i < device_count_; ++i) {
        bool match = true;
        for (int j = 0; j < 6; ++j) {
            if (devices_[i].mac[j] != mac[j]) {
                match = false;
                break;
            }
        }
        if (match)
            return &devices_[i];
    }

    // Add new
    if (device_count_ >= kMaxDevices)
        return nullptr;

    BleDeviceFingerprint* dev = &devices_[device_count_];
    for (int j = 0; j < 6; ++j)
        dev->mac[j] = mac[j];
    dev->addr_type = BleAddrType::Unknown;
    dev->device_class = BleDeviceClass::Unknown;
    dev->ad_flags = 0;
    dev->is_connectable = false;
    dev->is_scannable = false;
    dev->rssi_index = 0;
    dev->rssi_count = 0;
    dev->rssi_avg = rssi_index >= 0 ? dev->rssi_history[0] : 0;
    dev->rssi_max = -128;
    dev->rssi_min = 0;
    dev->name_len = 0;
    dev->name[0] = '\0';
    dev->appearance = 0;
    dev->tx_power_dbm = 0;
    dev->uuid16_count = 0;
    dev->company_id = 0;
    dev->manuf_data_len = 0;
    dev->first_seen_ms = tick_count;
    dev->has_sc_confirm = false;
    dev->has_sc_random = false;
    dev->has_oob_support = false;
    dev->has_simple_pairing = false;

    ++device_count_;
    return dev;
}

inline void BleScanner::parse_ad_structures_(BleDeviceFingerprint* dev, const uint8_t* data, uint8_t len) noexcept {
    uint8_t pos = 0;
    while (pos + 1 < len) {
        uint8_t field_len = data[pos];
        if (field_len == 0 || pos + 1 + field_len > len)
            break;
        uint8_t field_type = data[pos + 1];
        const uint8_t* field_data = data + pos + 2;
        uint8_t value_len = field_len - 1;

        switch (field_type) {
        case BleAdType::Flags:
            if (value_len >= 1) {
                dev->ad_flags = field_data[0];
                dev->is_connectable = (field_data[0] & BleAdFlag::GeneralDiscoverable) != 0;
                dev->is_scannable = true;
            }
            break;

        case BleAdType::ShortName:
        case BleAdType::CompleteName:
            if (value_len > 0) {
                dev->name_len =
                    (value_len < BleDeviceFingerprint::kMaxNameLen) ? value_len : BleDeviceFingerprint::kMaxNameLen;
                for (uint8_t k = 0; k < dev->name_len; ++k)
                    dev->name[k] = static_cast<char>(field_data[k]);
                dev->name[dev->name_len] = '\0';
            }
            break;

        case BleAdType::CompleteUuid16:
            dev->uuid16_count = value_len / 2;
            if (dev->uuid16_count > 4)
                dev->uuid16_count = 4;
            for (uint8_t k = 0; k < dev->uuid16_count; ++k) {
                dev->uuid16_list[k] =
                    static_cast<uint16_t>(field_data[k * 2]) | (static_cast<uint16_t>(field_data[k * 2 + 1]) << 8);
            }
            break;

        case BleAdType::Appearance:
            if (value_len >= 2) {
                dev->appearance = static_cast<uint16_t>(field_data[0]) | (static_cast<uint16_t>(field_data[1]) << 8);
            }
            break;

        case BleAdType::TxPowerLevel:
            if (value_len >= 1)
                dev->tx_power_dbm = static_cast<int8_t>(field_data[0]);
            break;

        case BleAdType::ManufacturerData:
            if (value_len >= 2) {
                dev->company_id = static_cast<uint16_t>(field_data[0]) | (static_cast<uint16_t>(field_data[1]) << 8);
                uint8_t md_len = value_len - 2;
                if (md_len > BleDeviceFingerprint::kMaxManufacturerData)
                    md_len = BleDeviceFingerprint::kMaxManufacturerData;
                for (uint8_t k = 0; k < md_len; ++k)
                    dev->manuf_data[k] = field_data[2 + k];
                dev->manuf_data_len = md_len;
            }
            break;

        case BleAdType::LeSecureConfirm:
            dev->has_sc_confirm = true;
            break;

        case BleAdType::LeSecureRandom:
            dev->has_sc_random = true;
            break;

        case BleAdType::SecurityManagerOobFlags:
            dev->has_oob_support = true;
            break;

        case BleAdType::SimplePairingHashC192:
        case BleAdType::SimplePairingHashC256:
            dev->has_simple_pairing = true;
            break;

        default:
            break;
        }

        pos += 1 + field_len;
    }
}

inline void BleScanner::update_rssi_(BleDeviceFingerprint* dev, int8_t rssi) noexcept {
    dev->rssi_history[dev->rssi_index] = rssi;
    dev->rssi_index = (dev->rssi_index + 1) % BleDeviceFingerprint::kMaxRssiSamples;
    if (dev->rssi_count < BleDeviceFingerprint::kMaxRssiSamples)
        ++dev->rssi_count;

    // Recompute avg/min/max
    int32_t sum = 0;
    int8_t mn = 0;
    int8_t mx = -128;
    for (uint8_t i = 0; i < dev->rssi_count; ++i) {
        sum += dev->rssi_history[i];
        if (dev->rssi_history[i] < mn)
            mn = dev->rssi_history[i];
        if (dev->rssi_history[i] > mx)
            mx = dev->rssi_history[i];
    }
    dev->rssi_avg = static_cast<int8_t>(sum / dev->rssi_count);
    dev->rssi_min = mn;
    dev->rssi_max = mx;
    dev->last_rssi_update_ms = tick_count;
}

inline void BleScanner::classify_device_(BleDeviceFingerprint* dev) noexcept {
    // Heuristic classification based on appearance + UUIDs + manufacturer

    // 1. Check Appearance field (most reliable)
    uint16_t cat = dev->appearance >> 6; // top 10 bits = category
    if (cat == (BleAppearance::GenericPhone >> 6)) {
        dev->device_class = BleDeviceClass::Phone;
        return;
    }
    if (cat == (BleAppearance::GenericWatch >> 6)) {
        dev->device_class = BleDeviceClass::Watch;
        return;
    }
    if (cat == (BleAppearance::GenericComputer >> 6)) {
        dev->device_class = BleDeviceClass::Computer;
        return;
    }
    if (cat == (BleAppearance::GenericTag >> 6)) {
        dev->device_class = BleDeviceClass::Tracker;
        return;
    }

    // 2. Check for known beacon patterns
    //    iBeacon: company_id=0x004C, Apple
    //    Eddystone: 0xFEAA Service UUID
    if (dev->company_id == 0x004C) {
        dev->device_class = BleDeviceClass::Beacon;
        return;
    }
    for (uint8_t i = 0; i < dev->uuid16_count; ++i) {
        if (dev->uuid16_list[i] == 0xFEAA) { // Eddystone
            dev->device_class = BleDeviceClass::Beacon;
            return;
        }
    }

    // 3. Fallback: Unknown
    dev->device_class = BleDeviceClass::Unknown;
}

inline void BleScanner::iter_ad_structures_(const uint8_t* data, uint8_t len, void (*cb)(const AdEntry&, void*),
                                            void* user) noexcept {
    if (!data || !cb)
        return;
    uint8_t pos = 0;
    while (pos + 1 < len) {
        uint8_t field_len = data[pos];
        if (field_len == 0 || pos + 1 + field_len > len)
            break;
        AdEntry entry;
        entry.type = data[pos + 1];
        entry.length = field_len - 1;
        entry.data = data + pos + 2;
        cb(entry, user);
        pos += 1 + field_len;
    }
}
