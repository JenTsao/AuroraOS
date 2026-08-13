// ============================================================
// ble_stealth.hpp — BLE 广播隐身伪装引擎 (Header-Only)
//
// 设备开启蓝牙后，自动伪装成周边最常见的智能小外设，
// 混入蓝牙无线电背景噪音中，避免被 IDS / 系统蓝牙
// 设置列表识别为安全审计目标。
//
// 两层伪装：
//   Layer 1: GAP Flags 隐藏 — 不广播 Limited/General Discoverable
//            位，周边手机系统蓝牙设置搜不到；主人通过物理 MAC
//            定向连接。
//   Layer 2: iBeacon 指纹欺骗 — 在 Manufacturer Specific Data
//            (AD Type 0xFF) 中将公司 ID 设为 0x004C (Apple)，
//            BLE IDS 将其归类为合法 Apple AirTag 追踪器。
//
// 构建的 AD 数据 ≤ 31 字节，直接注入 HalBle::start_advertising_raw()。
//
// References:
//   Bluetooth Core Spec v5.3, Vol 3 Part C §11 (AD types)
//   Apple iBeacon Specification (Proximity Beacon)
//   BLE GAP Advertising Data format
// ============================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace auroraos {
namespace ble {

// ---- BLE 隐身预设 ----
enum class BleStealthPreset : uint8_t {
    NONE = 0,         // 关闭伪装，正常广播 (GAP Discoverable + 真实设备名)
    AIRTAG = 1,       // Apple AirTag 追踪器
    AIRPODS_PRO = 2,  // Apple AirPods Pro
    AIRPODS = 3,      // Apple AirPods (标准版)
    APPLE_PENCIL = 4, // Apple Pencil (第 2 代)
};

// ---- BLE 广告包最大长度 (BLE 4.x/5.x 规范限制) ----
static constexpr size_t BLE_ADV_MAX_LEN = 31;

// ---- iBeacon 规范常量 ----
static constexpr uint16_t APPLE_COMPANY_ID = 0x004C;
static constexpr uint8_t IBEACON_TYPE = 0x02;
static constexpr uint8_t IBEACON_DATA_LEN = 0x15; // 21 bytes 固定

// ---- iBeacon 默认接近 UUID (auroraOS 自定义，可替换) ----
// AirTag 真实 UUID 为 Apple 私有；此处使用中性 UUID 模拟跟踪器行为
static constexpr uint8_t DEFAULT_IBEACON_UUID[16] = {0x74, 0x27, 0x8B, 0xDA, 0xE9, 0x44, 0x45, 0x2C,
                                                     0xA0, 0x95, 0x64, 0xC7, 0x2E, 0xA1, 0x3D, 0x17};

// ---- AirPods Pro 风格接近 UUID ----
static constexpr uint8_t AIRPODS_PRO_UUID[16] = {0x76, 0x31, 0xA9, 0xCA, 0x1D, 0x31, 0x43, 0xE9,
                                                 0xB9, 0x2C, 0x9F, 0x15, 0xF8, 0xD2, 0xEA, 0x42};

// ---- AirPods (标准版) 接近 UUID ----
static constexpr uint8_t AIRPODS_UUID[16] = {0x9D, 0x47, 0xF1, 0x3B, 0x5A, 0x2C, 0x48, 0x13,
                                             0x8F, 0xAE, 0x12, 0x3B, 0xE7, 0x0D, 0xC6, 0x11};

// ---- Apple Pencil 接近 UUID ----
static constexpr uint8_t APPLE_PENCIL_UUID[16] = {0xDA, 0x2B, 0x84, 0xF1, 0x62, 0x79, 0x4F, 0xBA,
                                                  0xB4, 0x81, 0x52, 0xDF, 0x3B, 0x1E, 0x34, 0x88};

// ---- iBeacon 默认 Major / Minor ----
static constexpr uint16_t DEFAULT_IBEACON_MAJOR = 0x0001;
static constexpr uint16_t DEFAULT_IBEACON_MINOR = 0x0001;

// ---- BLE GAP Flag Bits (Bluetooth Core Spec) ----
static constexpr uint8_t BLE_FLAG_LE_LIMITED_DISC = 0x01;
static constexpr uint8_t BLE_FLAG_LE_GENERAL_DISC = 0x02;
static constexpr uint8_t BLE_FLAG_BREDR_NOT_SUPPORTED = 0x04;
static constexpr uint8_t BLE_FLAG_SIMUL_LE_BREDR_CTRL = 0x08;
static constexpr uint8_t BLE_FLAG_SIMUL_LE_BREDR_HOST = 0x10;

// ============================================================
// BleStealth — BLE 广播隐身引擎 (单例)
// ============================================================
class BleStealth {
public:
    // ---- 预设名称表 (供 Shell 显示使用) ----
    static constexpr const char* preset_name(BleStealthPreset p) {
        switch (p) {
        case BleStealthPreset::AIRTAG:
            return "Apple AirTag";
        case BleStealthPreset::AIRPODS_PRO:
            return "Apple AirPods Pro";
        case BleStealthPreset::AIRPODS:
            return "Apple AirPods";
        case BleStealthPreset::APPLE_PENCIL:
            return "Apple Pencil";
        default:
            return "None";
        }
    }

    // ---- 单例 ----
    static BleStealth& instance() {
        static BleStealth s;
        return s;
    }

    void set_preset(BleStealthPreset p) {
        preset_ = p;
    }

    BleStealthPreset get_preset() const {
        return preset_;
    }

    // ---- 核心 API：构建隐身广播 AD 数据包 ----
    //
    // 将构建好的 ≤31 字节 AD 数据写入 out_ad_data，返回实际长度。
    // Layer 1 (GAP Flags 隐藏): 不设 Limited/General Discoverable，
    //     仅声明 BR/EDR Not Supported + SIMUL LE+BR/EDR (Host)。
    //     效果：系统蓝牙设置扫描搜不到，但定向连接 (by MAC) 仍然成功。
    // Layer 2 (iBeacon 指纹): Manufacturer Specific Data (0xFF)
    //     中公司 ID = 0x004C (Apple)，payload 符合 iBeacon 规范。
    //     效果：BLE IDS 将其标记为 Apple AirTag / AirPods 等无害设备。
    //
    // NONE 预设：仍构建合法 AD (Flags 含 Discoverable + 原设备名)。
    size_t build_advertisement(uint8_t* out_ad_data, size_t max_len, const uint8_t* mac = nullptr) {
        // ---- 清空缓冲区 ----
        memset(out_ad_data, 0, max_len);
        size_t pos = 0;

        // ========================================================
        // AD Structure 1: Flags (AD Type 0x01)
        // ========================================================
        // Length: 2 (1 type + 1 value)
        uint8_t flags_value = 0;
        if (preset_ == BleStealthPreset::NONE) {
            // 正常模式：LE General Discoverable + BR/EDR Not Supported
            flags_value = BLE_FLAG_LE_GENERAL_DISC | BLE_FLAG_BREDR_NOT_SUPPORTED;
        } else {
            // Layer 1 — 隐身模式：仅声明 BR/EDR Not Supported
            // 不设 Limited/General Discoverable，彻底从扫描结果中消失
            flags_value = BLE_FLAG_BREDR_NOT_SUPPORTED;
        }
        out_ad_data[pos++] = 0x02; // length = 2
        out_ad_data[pos++] = 0x01; // AD Type = Flags
        out_ad_data[pos++] = flags_value;

        // ========================================================
        // AD Structure 2 (隐身模式): Manufacturer Specific Data (AD Type 0xFF)
        // 将设备指纹从 "未知 BLE 外设" 改写为 "Apple iBeacon/AirTag"
        // ========================================================
        if (preset_ != BleStealthPreset::NONE) {
            const uint8_t* uuid = get_uuid_for_preset();
            uint16_t major = DEFAULT_IBEACON_MAJOR;
            uint16_t minor = DEFAULT_IBEACON_MINOR;
            int8_t tx_pwr = get_tx_power_for_preset();

            // --- 计算 Manufacturer Specific Data 长度 ---
            // 结构: Company ID(2) + iBeacon Type(1) + iBeacon Data Len(1)
            //       + UUID(16) + Major(2) + Minor(2) + TX Power(1) = 25 bytes
            // AD Length 字段 = 1 (AD Type) + 25 (payload) = 26 (0x1A)
            static constexpr uint8_t MFG_AD_LEN = 0x1A; // length after this byte
            static constexpr uint8_t MFG_PAYLOAD_LEN = 25;

            // 检查是否超出 BLE 广播包最大长度（31 字节）
            // Flags AD (3) + Mfg AD (1 + 26) = 30 ≤ 31 ✓
            if (pos + 1 + MFG_PAYLOAD_LEN > max_len) {
                return pos; // 缓冲区不足，只返回 Flags
            }

            // AD Length
            out_ad_data[pos++] = MFG_AD_LEN;
            // AD Type = Manufacturer Specific Data
            out_ad_data[pos++] = 0xFF;
            // Company Identifier (little-endian): 0x004C = Apple Inc.
            out_ad_data[pos++] = static_cast<uint8_t>(APPLE_COMPANY_ID & 0xFF); // 0x4C
            out_ad_data[pos++] = static_cast<uint8_t>(APPLE_COMPANY_ID >> 8);   // 0x00
            // iBeacon Type
            out_ad_data[pos++] = IBEACON_TYPE;
            // iBeacon Data Length (21 bytes remaining)
            out_ad_data[pos++] = IBEACON_DATA_LEN;
            // Proximity UUID (16 bytes, big-endian)
            memcpy(&out_ad_data[pos], uuid, 16);
            pos += 16;
            // Major (2 bytes, big-endian)
            out_ad_data[pos++] = static_cast<uint8_t>(major >> 8);
            out_ad_data[pos++] = static_cast<uint8_t>(major & 0xFF);
            // Minor (2 bytes, big-endian)
            out_ad_data[pos++] = static_cast<uint8_t>(minor >> 8);
            out_ad_data[pos++] = static_cast<uint8_t>(minor & 0xFF);
            // Measured TX Power at 1 meter (signed int8, 2's complement)
            out_ad_data[pos++] = static_cast<uint8_t>(tx_pwr);

            // 隐身模式下不广播设备名 (AD Type 0x08/0x09)。
            // Apple iBeacon 规范本身也不含名称字段，
            // 仅通过定向连接 (direct connection) 与主人的 APP 通信。
        } else {
            // 正常模式：不添加 Manufacturer Data，由上层 HalBle 构建设备名
        }

        return pos;
    }

    // ---- 暴露当前 MAC (从外部注入，用于 AirTag 追踪校验) ----
    void set_device_mac(const uint8_t* mac) {
        if (mac)
            memcpy(device_mac_, mac, 6);
    }

private:
    BleStealth() : preset_(BleStealthPreset::NONE) {
        memset(device_mac_, 0, sizeof(device_mac_));
    }

    BleStealthPreset preset_;
    uint8_t device_mac_[6];

    // ---- 按预设返回 iBeacon UUID ----
    const uint8_t* get_uuid_for_preset() const {
        switch (preset_) {
        case BleStealthPreset::AIRPODS_PRO:
            return AIRPODS_PRO_UUID;
        case BleStealthPreset::AIRPODS:
            return AIRPODS_UUID;
        case BleStealthPreset::APPLE_PENCIL:
            return APPLE_PENCIL_UUID;
        case BleStealthPreset::AIRTAG:
        default:
            return DEFAULT_IBEACON_UUID;
        }
    }

    // ---- 按预设返回 iBeacon TX Power (1m 参考 RSSI) ----
    // 典型值: AirTag ≈ -59 dBm, AirPods ≈ -54 dBm, Pencil ≈ -62 dBm
    int8_t get_tx_power_for_preset() const {
        switch (preset_) {
        case BleStealthPreset::AIRPODS_PRO:
            return -54;
        case BleStealthPreset::AIRPODS:
            return -55;
        case BleStealthPreset::APPLE_PENCIL:
            return -62;
        case BleStealthPreset::AIRTAG:
        default:
            return -59; // AirTag 典型值
        }
    }
};

// ---- 编译期 Consteval 工具：从 Kconfig 宏映射为枚举 ----
// 由 apps 层调用，在 BleManager::init() 前选择预设
inline BleStealthPreset ble_stealth_preset_from_config() {
#if defined(CONFIG_STEALTH_BLE_AIRTAG)
    return BleStealthPreset::AIRTAG;
#elif defined(CONFIG_STEALTH_BLE_AIRPODS_PRO)
    return BleStealthPreset::AIRPODS_PRO;
#elif defined(CONFIG_STEALTH_BLE_AIRPODS)
    return BleStealthPreset::AIRPODS;
#elif defined(CONFIG_STEALTH_BLE_APPLE_PENCIL)
    return BleStealthPreset::APPLE_PENCIL;
#elif defined(CONFIG_STEALTH_BLE_NONE)
    return BleStealthPreset::NONE;
#else
    return BleStealthPreset::NONE; // 未配置 Kconfig 时默认关闭
#endif
}

} // namespace ble
} // namespace auroraos
