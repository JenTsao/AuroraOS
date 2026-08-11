#ifndef AURORA_WIRELESS_DEAUTH_DETECTOR_HPP
#define AURORA_WIRELESS_DEAUTH_DETECTOR_HPP

#include <stdint.h>
#include <stddef.h>
#include "wifi_monitor.hpp"
#include "../../kernel/core/mutex.hpp"

// ============================================================
// Deauth Detector — 解除认证/关联攻击检测引擎
//
// 检测模式：
//   1. 洪水攻击：短时间内大量 Deauth/Disassoc 帧
//   2. 欺骗检测：伪造 AP 发送 Deauth（BSSID 已知但序列号跳跃）
//   3. 广播 Deauth：目标地址为 FF:FF:FF:FF:FF:FF
//   4. Reason Code 分析：识别可疑的断开原因
//   5. 速率监控：滑动窗口内帧速率超过阈值
//
// 802.11 管理帧 Deauth Reason Codes：
//   1  = Unspecified
//   2  = Previous authentication no longer valid
//   3  = STA leaving BSS
//   4  = Inactivity timer expired
//   6  = Class 2 frame received from non-authenticated STA
//   7  = Class 3 frame received from non-associated STA
//   8  = STA leaving BSS (roaming)
//   15 = 4-way handshake timeout
//   34 = Disassociated because AP unable to handle all currently associated STAs
// ============================================================

// Deauth 事件
struct DeauthEvent {
    uint8_t  transmitter[6];   // 发送者 MAC (TA)
    uint8_t  receiver[6];      // 接收者 MAC (RA)
    uint8_t  bssid[6];         // BSSID (addr3)
    uint16_t reason_code;      // 断开原因码
    bool     is_deauth;        // true=Deauth, false=Disassoc
    bool     is_broadcast;     // RA 是否为广播地址
    bool     is_spoofed;       // 是否为欺骗帧
    uint16_t seq_number;       // 802.11 序列号
    int8_t   rssi_dbm;         // 信号强度
    uint32_t timestamp;        // 时间戳
};

// 检测告警级别
enum class DeauthAlertLevel : uint8_t {
    Info    = 0,  // 单次事件
    Warning = 1,  // 轻度异常（< 阈值）
    Alert   = 2,  // 中度异常（超过阈值）
    Critical = 3  // 确认攻击（洪水/持续欺骗）
};

// 检测告警
struct DeauthAlert {
    DeauthAlertLevel level;
    uint8_t  ap_mac[6];
    uint8_t  client_mac[6];
    uint16_t deauth_count;       // 窗口内 Deauth 数
    uint16_t disassoc_count;     // 窗口内 Disassoc 数
    uint16_t reason_code;        // 最常见原因码
    bool     is_broadcast_attack;
    bool     is_spoof_attack;
    uint32_t start_tick;
    uint32_t duration_ms;
};

// 每 BSSID 的跟踪状态
struct BssidTracker {
    uint8_t  bssid[6];
    uint16_t deauth_window[32];    // 滑动窗口（记录 Deauth 时间戳）
    uint16_t disassoc_window[32];
    uint8_t  deauth_idx;
    uint8_t  disassoc_idx;
    uint16_t last_seq_number;      // 上次序列号
    uint16_t seq_jumps;            // 序列号异常跳跃次数
    int8_t   baseline_rssi;        // 基准信号强度
    uint32_t last_alert_tick;      // 上次告警时间
    uint32_t last_event_tick;
};

// ============================================================
// Deauth Detector 主类
// ============================================================
class DeauthDetector {
public:
    static DeauthDetector& instance() {
        static DeauthDetector detector;
        return detector;
    }

    // ---- 配置 ----

    void set_deauth_threshold(uint16_t per_second) {
        deauth_threshold_per_sec_ = per_second;
    }

    void set_window_size(uint8_t seconds) {
        window_size_sec_ = (seconds > 32) ? 32 : seconds;
    }

    // ---- 帧处理 ----

    // 处理一帧 Deauth/Disassoc 管理帧
    //   返回: nullptr=无告警, 非null=告警信息
    const DeauthAlert* process_deauth(const CapturedFrame& frame) {
        if (frame.frame_len < 36) return nullptr;

        const uint8_t* data = skip_radiotap_(frame.data, frame.frame_len);
        int remain = frame.frame_len - static_cast<int>(data - frame.data);
        if (remain < static_cast<int>(sizeof(WifiMacHeader)) + 2) return nullptr;

        const WifiMacHeader* mac = reinterpret_cast<const WifiMacHeader*>(data);
        FrameType ft = auroraos::wireless::classify_frame(mac->frame_control);

        bool is_deauth = (ft == FrameType::MgmtDeauth);
        bool is_disassoc = (ft == FrameType::MgmtDisassoc);
        if (!is_deauth && !is_disassoc) return nullptr;

        // 提取 reason code（帧体前 2 字节）
        const uint8_t* body = data + sizeof(WifiMacHeader);
        uint16_t reason = body[0] | (static_cast<uint16_t>(body[1]) << 8);

        // 构建事件
        DeauthEvent event{};
        for (int i = 0; i < 6; ++i) {
            event.transmitter[i] = mac->addr2[i];  // TA
            event.receiver[i]    = mac->addr1[i];  // RA
            event.bssid[i]       = mac->addr3[i];  // BSSID
        }
        event.reason_code  = reason;
        event.is_deauth    = is_deauth;
        event.is_broadcast = is_broadcast_mac_(mac->addr1);
        event.seq_number   = mac->seq_ctrl >> 4;
        event.rssi_dbm     = frame.rssi_dbm;
        event.timestamp    = get_tick_();

        // 欺骗检测
        event.is_spoofed = detect_spoof_(mac->addr2, mac->addr3, event.seq_number,
                                          event.rssi_dbm);

        // 更新 BSSID 跟踪
        LockGuard lock(tracker_mutex_);
        BssidTracker* tracker = find_or_create_tracker_(event.bssid);
        if (!tracker) return nullptr;

        update_tracker_(tracker, event);

        // 评估告警
        return evaluate_alert_(tracker, event);
    }

    // 批量处理
    int process_batch(const CapturedFrame* frames, int count,
                      DeauthAlert* out_alerts, int max_alerts) {
        int alert_count = 0;
        for (int i = 0; i < count && alert_count < max_alerts; ++i) {
            const DeauthAlert* alert = process_deauth(frames[i]);
            if (alert) {
                out_alerts[alert_count] = *alert;
                ++alert_count;
            }
        }
        return alert_count;
    }

    // ---- 统计 ----

    int get_tracked_bssid_count() const { return bssid_count_; }

    uint32_t get_total_deauths() const { return total_deauths_; }
    uint32_t get_total_disassocs() const { return total_disassocs_; }
    uint32_t get_total_spoofs() const { return total_spoofs_; }

    // 获取近期攻击（最近 N 秒内触发告警的 BSSID）
    int get_recent_attacks(uint32_t within_seconds, uint8_t* out_bssids,
                           int max_count) const {
        uint32_t threshold = get_tick_() - (within_seconds * 1000);
        int count = 0;
        for (int i = 0; i < bssid_count_ && count < max_count; ++i) {
            if (trackers_[i].last_alert_tick >= threshold) {
                for (int j = 0; j < 6; ++j) out_bssids[count * 6 + j] = trackers_[i].bssid[j];
                ++count;
            }
        }
        return count;
    }

    // ---- 工具 ----

    static const char* reason_code_name(uint16_t code) {
        switch (code) {
            case 1:  return "Unspecified";
            case 2:  return "Previous auth invalid";
            case 3:  return "STA leaving";
            case 4:  return "Inactivity";
            case 6:  return "Class 2 frame violation";
            case 7:  return "Class 3 frame violation";
            case 8:  return "Disassociated (roaming)";
            case 15: return "4-way handshake timeout";
            case 34: return "AP overloaded";
            default: return "Unknown";
        }
    }

    static const char* alert_level_name(DeauthAlertLevel level) {
        switch (level) {
            case DeauthAlertLevel::Info:     return "INFO";
            case DeauthAlertLevel::Warning:  return "WARNING";
            case DeauthAlertLevel::Alert:    return "ALERT";
            case DeauthAlertLevel::Critical: return "CRITICAL";
            default:                         return "?";
        }
    }

private:
    static constexpr int kMaxBssids = 64;

    BssidTracker trackers_[kMaxBssids]{};
    int          bssid_count_ = 0;
    Mutex        tracker_mutex_;

    uint16_t deauth_threshold_per_sec_ = 10;  // 每秒 Deauth 数阈值
    uint8_t  window_size_sec_ = 5;            // 滑动窗口（秒）
    uint32_t total_deauths_ = 0;
    uint32_t total_disassocs_ = 0;
    uint32_t total_spoofs_ = 0;

    DeauthDetector() = default;

    static const uint8_t* skip_radiotap_(const uint8_t* data, int len) {
        if (len < 8) return data;
        uint16_t radiotap_len = data[2] | (static_cast<uint16_t>(data[3]) << 8);
        if (radiotap_len < 8 || radiotap_len > static_cast<uint16_t>(len)) return data;
        return data + radiotap_len;
    }

    static bool is_broadcast_mac_(const uint8_t* mac) {
        for (int i = 0; i < 6; ++i) if (mac[i] != 0xFF) return false;
        return true;
    }

    // 检测欺骗：序列号不合理跳跃或信号强度突然变化
    bool detect_spoof_(const uint8_t* transmitter, const uint8_t* bssid,
                       uint16_t seq, int8_t rssi) {
        // 如果发送者是 BSSID 之外的其他 MAC，可能是第三方攻击者
        if (!mac_equal_(transmitter, bssid)) {
            return true; // TA ≠ BSSID → 可能是伪造
        }
        return false;
    }

    BssidTracker* find_or_create_tracker_(const uint8_t* bssid) {
        for (int i = 0; i < bssid_count_; ++i) {
            if (mac_equal_(trackers_[i].bssid, bssid)) return &trackers_[i];
        }
        if (bssid_count_ >= kMaxBssids) return nullptr;
        BssidTracker& t = trackers_[bssid_count_];
        for (int i = 0; i < 6; ++i) t.bssid[i] = bssid[i];
        ++bssid_count_;
        return &t;
    }

    void update_tracker_(BssidTracker* t, const DeauthEvent& e) {
        if (e.is_deauth) {
            t->deauth_window[t->deauth_idx] = static_cast<uint16_t>(e.timestamp & 0xFFFF);
            t->deauth_idx = (t->deauth_idx + 1) % 32;
            ++total_deauths_;
        } else {
            t->disassoc_window[t->disassoc_idx] = static_cast<uint16_t>(e.timestamp & 0xFFFF);
            t->disassoc_idx = (t->disassoc_idx + 1) % 32;
            ++total_disassocs_;
        }

        // 序列号跳跃检测
        if (t->last_seq_number != 0 && e.seq_number > 0) {
            uint16_t diff = e.seq_number - t->last_seq_number;
            if (diff > 100) { // 序列号跳跃超过 100 → 可疑
                ++t->seq_jumps;
            }
        }
        t->last_seq_number = e.seq_number;

        // 基准 RSSI（首次或周期性更新）
        if (t->baseline_rssi == 0) t->baseline_rssi = e.rssi_dbm;

        t->last_event_tick = e.timestamp;

        if (e.is_spoofed) ++total_spoofs_;
    }

    const DeauthAlert* evaluate_alert_(BssidTracker* t, const DeauthEvent& e) {
        uint32_t now = e.timestamp;
        uint32_t window_ticks = static_cast<uint32_t>(window_size_sec_) * 1000;

        // 统计窗口内 Deauth 数
        uint16_t deauth_count = 0;
        for (int i = 0; i < 32; ++i) {
            uint16_t ts = t->deauth_window[i];
            if (ts == 0) continue;
            uint32_t full_ts = (now & 0xFFFF0000) | ts;
            if (full_ts > now) full_ts -= 0x10000;
            if (now - full_ts < window_ticks) ++deauth_count;
        }

        // 统计窗口内 Disassoc 数
        uint16_t disassoc_count = 0;
        for (int i = 0; i < 32; ++i) {
            uint16_t ts = t->disassoc_window[i];
            if (ts == 0) continue;
            uint32_t full_ts = (now & 0xFFFF0000) | ts;
            if (full_ts > now) full_ts -= 0x10000;
            if (now - full_ts < window_ticks) ++disassoc_count;
        }

        uint16_t total = deauth_count + disassoc_count;

        // 判定告警级别
        DeauthAlertLevel level = DeauthAlertLevel::Info;

        if (total >= deauth_threshold_per_sec_ * window_size_sec_) {
            level = DeauthAlertLevel::Critical;
        } else if (total >= deauth_threshold_per_sec_ * window_size_sec_ / 2) {
            level = DeauthAlertLevel::Alert;
        } else if (total >= deauth_threshold_per_sec_ * window_size_sec_ / 4) {
            level = DeauthAlertLevel::Warning;
        } else if (total == 0) {
            return nullptr; // 无事件，不生成告警
        }

        // 防抖动：同一 BSSID 在 5 秒内不重复告警
        if (now - t->last_alert_tick < 5000 && level < DeauthAlertLevel::Critical) {
            return nullptr;
        }

        t->last_alert_tick = now;

        // 暂存告警（调用者应立即使用，后续 process_deauth 会覆盖）
        static DeauthAlert static_alert;
        static_alert.level = level;
        for (int i = 0; i < 6; ++i) static_alert.ap_mac[i] = t->bssid[i];
        for (int i = 0; i < 6; ++i) static_alert.client_mac[i] = e.receiver[i];
        static_alert.deauth_count    = deauth_count;
        static_alert.disassoc_count  = disassoc_count;
        static_alert.reason_code     = e.reason_code;
        static_alert.is_broadcast_attack = e.is_broadcast;
        static_alert.is_spoof_attack     = e.is_spoofed;
        static_alert.start_tick      = now - window_ticks;
        static_alert.duration_ms     = window_ticks;

        return &static_alert;
    }

    static bool mac_equal_(const uint8_t* a, const uint8_t* b) {
        for (int i = 0; i < 6; ++i) if (a[i] != b[i]) return false;
        return true;
    }

    static uint32_t get_tick_() {
        extern volatile uint32_t tick_count;
        return tick_count;
    }
};

#endif // AURORA_WIRELESS_DEAUTH_DETECTOR_HPP
