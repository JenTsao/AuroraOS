#ifndef AURORA_WIRELESS_BEACON_ANALYZER_HPP
#define AURORA_WIRELESS_BEACON_ANALYZER_HPP

#include <stdint.h>
#include <stddef.h>
#include "wifi_monitor.hpp"

// ============================================================
// Beacon Analyzer — 信标帧分析引擎
//
// 功能：
//   1. 解析 802.11 Beacon 帧中的 IE (Information Elements)
//   2. 检测隐藏 SSID（length=0 或全零）
//   3. 检测弱加密（WEP, WPA-TKIP, 开放网络）
//   4. 检测 Rogue AP（同 SSID 不同 BSSID 或信道异常）
//   5. 构建 AP 环境表（BSSID→SSID→加密→信道 映射）
//   6. 企业级特性检测（802.1X, WPA3, PMF, 802.11w）
// ============================================================

// 加密/认证类型
enum class EncryptionType : uint8_t {
    Open           = 0,  // 无加密
    Wep            = 1,  // WEP (已破解)
    WpaPskTkip     = 2,  // WPA-PSK TKIP (弱)
    WpaPskCcmp     = 3,  // WPA-PSK AES-CCMP
    Wpa2PskTkip    = 4,  // WPA2-PSK TKIP (弱)
    Wpa2PskCcmp    = 5,  // WPA2-PSK AES-CCMP (强)
    Wpa2Enterprise = 6,  // WPA2-Enterprise (802.1X)
    Wpa3Sae        = 7,  // WPA3-SAE (最强)
    Wpa3Enterprise = 8,  // WPA3-Enterprise
    Mixed          = 9,  // WPA/WPA2 混合模式
    Unknown        = 10
};

// 信标帧中发现的 AP 信息
struct ApInfo {
    uint8_t  bssid[6];           // AP MAC
    char     ssid[33];           // SSID (最大 32 字节 + '\0')
    uint8_t  ssid_len;           // 实际 SSID 长度
    uint8_t  channel;            // 信道号
    EncryptionType encryption;   // 加密类型
    int8_t   rssi_dbm;           // 信号强度
    uint16_t beacon_interval;    // Beacon 间隔 (TU, 1 TU=1024us)
    uint8_t  capabilities[2];    // Capability Info
    bool     is_hidden;          // SSID 是否隐藏
    bool     is_wps_supported;   // 支持 WPS
    bool     is_pmf_required;    // 要求 PMF (Protected Management Frames)
    bool     is_transition_mode; // WPA3 Transition Mode
    bool     is_80211w_capable;  // 支持 802.11w
    uint32_t last_seen_tick;     // 最后出现时间戳
    uint32_t first_seen_tick;    // 首次发现时间戳
    uint16_t beacon_count;       // 收到的信标数
    char     vendor_oui[9];      // 厂商 OUI (前 3 字节 MAC)
};

// IE (Information Element) 标签 ID
enum class IeTag : uint8_t {
    Ssid            = 0,
    SupportedRates  = 1,
    DsParameter     = 3,
    TrafficIndication = 5,
    Country         = 7,
    PowerConstraint = 32,
    TpcReport       = 35,
    Erp             = 42,
    HtCapabilities  = 45,
    Rsn             = 48,   // WPA2
    ExtSupportedRates = 50,
    VhtCapabilities = 191,
    VhtOperation    = 192,
    VendorSpecific  = 221,
    Wpa             = 221,  // WPA1 (Vendor Specific 中 OUI=00:50:F2:01)
};

// Beacon 帧体偏移量（不含 802.11 MAC 头，从帧体开始计）
struct BeaconFrameBody {
    static constexpr int TIMESTAMP_OFFSET    = 0;   // 8 bytes
    static constexpr int BEACON_INTERVAL_OFF = 8;   // 2 bytes
    static constexpr int CAPABILITY_OFF      = 10;  // 2 bytes
    static constexpr int IE_START_OFF        = 12;  // 变长 IE 开始
};

// ============================================================
// Beacon Analyzer 主类
// ============================================================
class BeaconAnalyzer {
public:
    static BeaconAnalyzer& instance() {
        static BeaconAnalyzer analyzer;
        return analyzer;
    }

    // ---- 帧处理 ----

    // 处理一帧捕获的 Beacon
    //   返回: true=成功解析并加入AP表
    bool process_beacon(const CapturedFrame& frame) {
        if (frame.frame_len < 36) return false; // 最小 Beacon 帧长

        // 跳过 Radiotap 头
        const uint8_t* data = skip_radiotap_(frame.data, frame.frame_len);
        int remain = frame.frame_len - static_cast<int>(data - frame.data);
        if (remain < 36) return false;

        // 解析 802.11 MAC 头
        const WifiMacHeader* mac = reinterpret_cast<const WifiMacHeader*>(data);
        FrameType ft = auroraos::wireless::classify_frame(mac->frame_control);
        if (ft != FrameType::MgmtBeacon) return false;

        // 指针移到帧体
        const uint8_t* body = data + sizeof(WifiMacHeader);
        int body_len = remain - static_cast<int>(sizeof(WifiMacHeader));
        if (body_len < 12) return false;

        return parse_beacon_body_(frame, mac, body, body_len);
    }

    // 批量处理（从捕获缓冲区）
    int process_batch(const CapturedFrame* frames, int count) {
        int beacons = 0;
        for (int i = 0; i < count; ++i) {
            if (process_beacon(frames[i])) {
                ++beacons;
            }
        }
        return beacons;
    }

    // ---- AP 表查询 ----

    // 获取已知 AP 数量
    int get_ap_count() const { return ap_count_; }

    // 按索引获取 AP 信息
    const ApInfo* get_ap(int index) const {
        if (index < 0 || index >= ap_count_) return nullptr;
        return &ap_table_[index];
    }

    // 按 BSSID 查找
    const ApInfo* find_by_bssid(const uint8_t* bssid) const {
        for (int i = 0; i < ap_count_; ++i) {
            if (mac_equal_(ap_table_[i].bssid, bssid)) {
                return &ap_table_[i];
            }
        }
        return nullptr;
    }

    // ---- 安全分析 ----

    // 检测隐藏 SSID 的 AP 数量
    int count_hidden_ssid() const {
        int count = 0;
        for (int i = 0; i < ap_count_; ++i) {
            if (ap_table_[i].is_hidden) ++count;
        }
        return count;
    }

    // 检测弱加密 AP（WEP / WPA-TKIP / Open）
    int count_weak_encryption() const {
        int count = 0;
        for (int i = 0; i < ap_count_; ++i) {
            switch (ap_table_[i].encryption) {
                case EncryptionType::Open:
                case EncryptionType::Wep:
                case EncryptionType::WpaPskTkip:
                case EncryptionType::Wpa2PskTkip:
                    ++count;
                    break;
                default: break;
            }
        }
        return count;
    }

    // 检测 Rogue AP（相同 SSID 但不同 BSSID 或出现在异常信道）
    struct RogueApAlert {
        uint8_t  bssid_a[6];
        uint8_t  bssid_b[6];
        char     ssid[33];
        uint16_t channel_a;
        uint16_t channel_b;
        uint32_t first_seen;
    };

    int detect_rogue_aps(RogueApAlert* out_alerts, int max_alerts) const {
        int alert_count = 0;
        for (int i = 0; i < ap_count_ && alert_count < max_alerts; ++i) {
            if (ap_table_[i].ssid_len == 0) continue;
            for (int j = i + 1; j < ap_count_ && alert_count < max_alerts; ++j) {
                if (ap_table_[j].ssid_len == 0) continue;
                // 同 SSID 不同 BSSID 且不同信道 → 可疑
                if (ssid_equal_(ap_table_[i].ssid, ap_table_[j].ssid) &&
                    !mac_equal_(ap_table_[i].bssid, ap_table_[j].bssid) &&
                    ap_table_[i].channel != ap_table_[j].channel) {
                    RogueApAlert& alert = out_alerts[alert_count];
                    for (int k = 0; k < 6; ++k) {
                        alert.bssid_a[k] = ap_table_[i].bssid[k];
                        alert.bssid_b[k] = ap_table_[j].bssid[k];
                    }
                    copy_ssid_(alert.ssid, ap_table_[i].ssid, ap_table_[i].ssid_len);
                    alert.channel_a = ap_table_[i].channel;
                    alert.channel_b = ap_table_[j].channel;
                    alert.first_seen = ap_table_[j].first_seen_tick;
                    ++alert_count;
                }
            }
        }
        return alert_count;
    }

    // 获取加密统计摘要
    void get_encryption_summary(int out_counts[11]) const {
        for (int i = 0; i < 11; ++i) out_counts[i] = 0;
        for (int i = 0; i < ap_count_; ++i) {
            uint8_t idx = static_cast<uint8_t>(ap_table_[i].encryption);
            if (idx < 11) ++out_counts[idx];
        }
    }

    // ---- 工具方法 ----

    static const char* encryption_name(EncryptionType t) {
        switch (t) {
            case EncryptionType::Open:           return "Open";
            case EncryptionType::Wep:            return "WEP";
            case EncryptionType::WpaPskTkip:     return "WPA-TKIP";
            case EncryptionType::WpaPskCcmp:     return "WPA-AES";
            case EncryptionType::Wpa2PskTkip:    return "WPA2-TKIP";
            case EncryptionType::Wpa2PskCcmp:    return "WPA2-AES";
            case EncryptionType::Wpa2Enterprise: return "WPA2-Enterprise";
            case EncryptionType::Wpa3Sae:        return "WPA3-SAE";
            case EncryptionType::Wpa3Enterprise: return "WPA3-Enterprise";
            case EncryptionType::Mixed:          return "Mixed";
            default:                              return "Unknown";
        }
    }

    static bool is_weak_encryption(EncryptionType t) {
        return t == EncryptionType::Open ||
               t == EncryptionType::Wep  ||
               t == EncryptionType::WpaPskTkip ||
               t == EncryptionType::Wpa2PskTkip;
    }

private:
    static constexpr int kMaxAps = 128;
    static constexpr int kStaleTimeoutTicks = 300000; // 5min @1kHz

    ApInfo ap_table_[kMaxAps]{};
    int    ap_count_ = 0;
    Mutex  table_mutex_;

    BeaconAnalyzer() = default;

    // 跳过 Radiotap 头
    static const uint8_t* skip_radiotap_(const uint8_t* data, int len) {
        if (len < 8) return data;
        uint16_t radiotap_len = data[2] | (static_cast<uint16_t>(data[3]) << 8);
        if (radiotap_len < 8 || radiotap_len > static_cast<uint16_t>(len)) return data;
        return data + radiotap_len;
    }

    // 解析 Beacon 帧体
    bool parse_beacon_body_(const CapturedFrame& frame, const WifiMacHeader* mac,
                            const uint8_t* body, int body_len) {
        ApInfo info{};
        for (int i = 0; i < 6; ++i) info.bssid[i] = mac->addr3[i];
        info.rssi_dbm = frame.rssi_dbm;
        info.beacon_interval = body[BeaconFrameBody::BEACON_INTERVAL_OFF] |
                               (static_cast<uint16_t>(body[BeaconFrameBody::BEACON_INTERVAL_OFF + 1]) << 8);
        info.capabilities[0] = body[BeaconFrameBody::CAPABILITY_OFF];
        info.capabilities[1] = body[BeaconFrameBody::CAPABILITY_OFF + 1];

        info.ssid_len = 0;
        info.ssid[0] = '\0';
        info.is_hidden = false;
        info.encryption = EncryptionType::Open;
        info.is_wps_supported = false;
        info.is_pmf_required = false;
        info.is_transition_mode = false;
        info.is_80211w_capable = false;

        // 遍历 IE（从帧体偏移 12+ 开始）
        int offset = BeaconFrameBody::IE_START_OFF;
        bool has_rsn = false;
        bool has_wpa1 = false;

        while (offset + 2 <= body_len) {
            uint8_t tag    = body[offset];
            uint8_t ie_len = body[offset + 1];
            if (offset + 2 + ie_len > body_len) break;

            switch (tag) {
                case static_cast<uint8_t>(IeTag::Ssid):
                    if (ie_len == 0 || (ie_len > 0 && body[offset + 2] == 0x00)) {
                        info.is_hidden = true;
                    }
                    if (ie_len > 0 && ie_len <= 32) {
                        info.ssid_len = (ie_len > 32) ? 32 : ie_len;
                        for (uint8_t k = 0; k < info.ssid_len; ++k) {
                            info.ssid[k] = static_cast<char>(body[offset + 2 + k]);
                        }
                        info.ssid[info.ssid_len] = '\0';
                    }
                    break;

                case static_cast<uint8_t>(IeTag::DsParameter):
                    if (ie_len >= 1) info.channel = body[offset + 2];
                    break;

                case static_cast<uint8_t>(IeTag::Rsn):
                    has_rsn = true;
                    if (ie_len >= 2) {
                        parse_rsn_ie_(body + offset + 2, ie_len, info);
                    }
                    break;

                case static_cast<uint8_t>(IeTag::VendorSpecific):
                    parse_vendor_ie_(body + offset + 2, ie_len, info, has_wpa1);
                    break;
            }

            offset += 2 + ie_len;
        }

        // 加密类型判定
        if (has_rsn) {
            info.encryption = EncryptionType::Wpa2PskCcmp; // 默认，RSN IE 解析可能修正
        } else if (has_wpa1) {
            info.encryption = EncryptionType::WpaPskCcmp;
        } else if (info.capabilities[0] & 0x10) {
            info.encryption = EncryptionType::Wep;
        }

        // 分配厂商 OUI（BSSID 前 3 字节）
        format_oui_(info.bssid, info.vendor_oui);

        // 更新 AP 表
        LockGuard lock(table_mutex_);
        return upsert_ap_(info);
    }

    // 解析 RSN IE（WPA2/3 信息）
    void parse_rsn_ie_(const uint8_t* data, uint8_t len, ApInfo& info) {
        if (len < 2) return;
        uint16_t version = data[0] | (static_cast<uint16_t>(data[1]) << 8);

        // AKM Suite Count 偏移取决于版本
        int pos = 2; // version 后面是 Group Cipher Suite (4 bytes)
        if (pos + 4 > len) return;
        pos += 4; // 跳过 Group Cipher Suite

        if (pos + 2 > len) return;
        uint16_t pairwise_count = data[pos] | (static_cast<uint16_t>(data[pos + 1]) << 8);
        pos += 2 + static_cast<int>(pairwise_count) * 4;

        if (pos + 2 > len) return;
        uint16_t akm_count = data[pos] | (static_cast<uint16_t>(data[pos + 1]) << 8);
        pos += 2;

        bool has_sae = false;
        bool has_psk = false;
        bool has_8021x = false;

        for (int i = 0; i < static_cast<int>(akm_count) && pos + 4 <= len; ++i) {
            uint32_t oui = (static_cast<uint32_t>(data[pos]) << 16) |
                           (static_cast<uint32_t>(data[pos + 1]) << 8) |
                           data[pos + 2];
            uint8_t suite_type = data[pos + 3];

            if (oui == 0x000FAC) {
                switch (suite_type) {
                    case 2: has_psk = true;   break;
                    case 1: has_8021x = true; break;
                    case 8: has_sae = true;   break; // SAE (WPA3)
                    case 9: has_8021x = true; break;  // FT
                }
            }
            pos += 4;
        }

        // 判定加密
        if (has_sae) {
            info.encryption = EncryptionType::Wpa3Sae;
        } else if (has_8021x) {
            info.encryption = EncryptionType::Wpa2Enterprise;
        } else if (has_psk) {
            info.encryption = EncryptionType::Wpa2PskCcmp;
        }

        // RSN Capabilities（AKM 之后 2 字节）
        if (pos + 2 <= len) {
            uint16_t rsn_caps = data[pos] | (static_cast<uint16_t>(data[pos + 1]) << 8);
            info.is_pmf_required = (rsn_caps & 0x0040) != 0;   // MFPR
            info.is_80211w_capable = (rsn_caps & 0x0080) != 0; // MFPC
        }
    }

    // 解析 Vendor Specific IE（检测 WPA1 / WPS / WPA3 Transition）
    void parse_vendor_ie_(const uint8_t* data, uint8_t len, ApInfo& info, bool& has_wpa1) {
        if (len < 4) return;

        // 检测 WPA1 的 OUI: 00:50:F2:01
        if (data[0] == 0x00 && data[1] == 0x50 && data[2] == 0xF2) {
            if (data[3] == 0x01) {
                has_wpa1 = true;
            }
        }

        // 检测 WPS OUI: 00:50:F2:04
        if (data[0] == 0x00 && data[1] == 0x50 && data[2] == 0xF2 && data[3] == 0x04) {
            info.is_wps_supported = true;
        }

        // 检测 WPA3 Transition Mode OUI: 50:6F:9A:16
        if (data[0] == 0x50 && data[1] == 0x6F && data[2] == 0x9A && data[3] == 0x16) {
            info.is_transition_mode = true;
        }
    }

    // 更新或插入 AP 表
    bool upsert_ap_(const ApInfo& info) {
        // 查找已存在的 AP
        for (int i = 0; i < ap_count_; ++i) {
            if (mac_equal_(ap_table_[i].bssid, info.bssid)) {
                // 更新
                ap_table_[i].rssi_dbm  = info.rssi_dbm;
                ap_table_[i].last_seen_tick = get_tick_();
                ap_table_[i].beacon_count++;
                if (!ap_table_[i].is_hidden && info.ssid_len > 0) {
                    copy_ssid_(ap_table_[i].ssid, info.ssid, info.ssid_len);
                    ap_table_[i].ssid_len = info.ssid_len;
                }
                return true;
            }
        }

        // 清理过期条目
        uint32_t now = get_tick_();
        int write_idx = 0;
        for (int i = 0; i < ap_count_; ++i) {
            if (now - ap_table_[i].last_seen_tick < kStaleTimeoutTicks) {
                if (write_idx != i) {
                    ap_table_[write_idx] = ap_table_[i];
                }
                ++write_idx;
            }
        }
        ap_count_ = write_idx;

        // 插入新条目
        if (ap_count_ >= kMaxAps) return false;

        ap_table_[ap_count_] = info;
        ap_table_[ap_count_].first_seen_tick = now;
        ap_table_[ap_count_].last_seen_tick = now;
        ap_table_[ap_count_].beacon_count = 1;
        ++ap_count_;
        return true;
    }

    static bool mac_equal_(const uint8_t* a, const uint8_t* b) {
        for (int i = 0; i < 6; ++i) {
            if (a[i] != b[i]) return false;
        }
        return true;
    }

    static bool ssid_equal_(const char* a, const char* b) {
        int i = 0;
        while (i < 32) {
            if (a[i] != b[i]) return false;
            if (a[i] == '\0') return true;
            ++i;
        }
        return true;
    }

    static void copy_ssid_(char* dst, const char* src, uint8_t len) {
        uint8_t n = (len > 32) ? 32 : len;
        for (uint8_t i = 0; i < n; ++i) dst[i] = src[i];
        dst[n] = '\0';
    }

    static void format_oui_(const uint8_t* mac, char* out) {
        for (int i = 0; i < 8; ++i) out[i] = '\0';
        auto hex = [](uint8_t b) -> char {
            return (b < 10) ? ('0' + b) : ('A' + b - 10);
        };
        out[0] = hex(mac[0] >> 4); out[1] = hex(mac[0] & 0x0F); out[2] = ':';
        out[3] = hex(mac[1] >> 4); out[4] = hex(mac[1] & 0x0F); out[5] = ':';
        out[6] = hex(mac[2] >> 4); out[7] = hex(mac[2] & 0x0F);
    }

    static uint32_t get_tick_() {
        extern volatile uint32_t tick_count;
        return tick_count;
    }
};

#endif // AURORA_WIRELESS_BEACON_ANALYZER_HPP
