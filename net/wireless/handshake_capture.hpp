#ifndef AURORA_WIRELESS_HANDSHAKE_CAPTURE_HPP
#define AURORA_WIRELESS_HANDSHAKE_CAPTURE_HPP

#include <stdint.h>
#include <stddef.h>
#include "wifi_monitor.hpp"
#include "../../kernel/core/mutex.hpp"

// ============================================================
// Handshake Capture — WPA/WPA2 四次握手捕获引擎
//
// 工作流程：
//   1. 监控 EAPOL 帧（EtherType 0x888E）
//   2. 跟踪每对 STA↔AP 的状态机
//   3. Message 1: AP→STA  (ANonce)
//   4. Message 2: STA→AP  (SNonce + MIC)
//   5. Message 3: AP→STA  (GTK + MIC)
//   6. Message 4: STA→AP  (ACK)
//   7. 完整握手捕获后提取 PMKID（可选）和 Nonce 对
//   8. 输出 pcapng 兼容格式（供 hashcat/aircrack-ng 离线破解）
// ============================================================

// EAPOL 帧头部
struct __attribute__((packed)) EapolHeader {
    uint8_t  version;      // 1 = 802.1X-2001, 2 = 802.1X-2004
    uint8_t  packet_type;  // 0=EAP, 1=EAPOL-Start, 2=EAPOL-Logoff, 3=EAPOL-Key
    uint16_t body_length;  // 负载长度

    // EAPOL-Key 子结构（当 packet_type=3）
    // uint8_t  key_descriptor_type;  // 2 = WPA, 254 = WPA2
    // uint16_t key_info;             // 关键标志位
    // uint16_t key_length;
    // uint8_t  key_replay_counter[8];
    // uint8_t  key_nonce[32];
    // uint8_t  key_iv[16];
    // uint8_t  key_rsc[8];
    // uint8_t  key_id[8];
    // uint8_t  key_mic[16];
    // uint16_t key_data_length;
    // uint8_t  key_data[...];        // RSN IE 等
};

// EAPOL-Key Key Info 位掩码
namespace KeyInfoBits {
    constexpr uint16_t KeyDescriptorVersion = 0x0007;  // bits 0-2
    constexpr uint16_t KeyType            = 0x0008;    // bit 3: 0=Group, 1=Pairwise
    constexpr uint16_t KeyIndex           = 0x0030;    // bits 4-5
    constexpr uint16_t Install            = 0x0040;    // bit 6
    constexpr uint16_t KeyAck             = 0x0080;    // bit 7
    constexpr uint16_t KeyMic             = 0x0100;    // bit 8
    constexpr uint16_t Secure             = 0x0200;    // bit 9
    constexpr uint16_t Error              = 0x0400;    // bit 10
    constexpr uint16_t Request            = 0x0800;    // bit 11
    constexpr uint16_t EncryptedKeyData   = 0x1000;    // bit 12
    constexpr uint16_t SmkMessage         = 0x2000;    // bit 13
}

// 四次握手消息类型
enum class HandshakeMessage : uint8_t {
    None    = 0,
    M1      = 1,   // AP→STA, ANonce, KeyAck
    M2      = 2,   // STA→AP, SNonce, KeyMic
    M3      = 3,   // AP→STA, KeyAck+KeyMic+Install+Encrypted
    M4      = 4,   // STA→AP, KeyMic+Secure
};

// 握手会话（一对 STA↔AP）
struct HandshakeSession {
    uint8_t  sta_mac[6];           // STA MAC
    uint8_t  ap_mac[6];            // AP MAC (BSSID)
    char     ssid[33];             // AP SSID

    // EAPOL 帧原始数据（供外部工具导出）
    uint8_t  eapol_m1[256];       // Message 1 完整帧
    uint8_t  eapol_m2[256];       // Message 2 完整帧
    uint8_t  eapol_m3[256];       // Message 3 完整帧
    uint8_t  eapol_m4[256];       // Message 4 完整帧
    uint16_t eapol_m1_len;
    uint16_t eapol_m2_len;
    uint16_t eapol_m3_len;
    uint16_t eapol_m4_len;

    // Nonce（提取自 EAPOL-Key）
    uint8_t  anonce[32];          // AP Nonce (M1)
    uint8_t  snonce[32];          // STA Nonce (M2)
    bool     anonce_valid;
    bool     snonce_valid;

    // MIC（供离线破解验证）
    uint8_t  mic_m2[16];          // Message 2 MIC
    uint8_t  mic_m3[16];          // Message 3 MIC

    // PMKID（从 M1/M2 的 RSN IE 提取）
    uint8_t  pmkid[16];
    bool     pmkid_valid;

    // 状态
    HandshakeMessage last_message;  // 最后收到的消息
    uint8_t  key_descriptor;        // 2=WPA, 254=WPA2
    uint8_t  missed_messages;       // 缺失消息计数
    bool     complete;              // 完整 M1-M4 握手
    bool     pmkid_capture;         // 仅 PMKID 捕获（无需完整握手）

    uint32_t first_seen_tick;
    uint32_t last_seen_tick;
};

// ============================================================
// Handshake Capture 主类
// ============================================================
class HandshakeCapture {
public:
    static HandshakeCapture& instance() {
        static HandshakeCapture capture;
        return capture;
    }

    // ---- 帧处理 ----

    // 处理一帧 EAPOL（由 wifi_monitor 回调调用）
    //   返回: 0=未处理, 1=握手进行中, 2=握手完成
    int process_eapol(const CapturedFrame& frame) {
        if (frame.frame_len < 48) return 0;

        // 跳过 Radiotap → 802.11 头
        const uint8_t* data = skip_radiotap_(frame.data, frame.frame_len);
        int remain = frame.frame_len - static_cast<int>(data - frame.data);
        if (remain < static_cast<int>(sizeof(WifiMacHeader)) + 4) return 0;

        const WifiMacHeader* mac = reinterpret_cast<const WifiMacHeader*>(data);

        // 检查是否为 Data 帧（EAPOL 通过 Data 帧发送）
        FrameType ft = auroraos::wireless::classify_frame(mac->frame_control);
        if (ft != FrameType::Data && ft != FrameType::DataQos) return 0;

        // 跳过 802.11 头 → LLC/SNAP → EAPOL
        int l2_offset = sizeof(WifiMacHeader);
        // QoS Data 有额外的 QoS Control 字段（2 字节）
        if (ft == FrameType::DataQos) l2_offset += 2;

        if (l2_offset + 14 > remain) return 0; // LLC(8) + SNAP(6) 最小值

        const uint8_t* llc = data + l2_offset;
        // 检查 LLC+SNAP 标识 EAPOL
        // LLC: AA AA 03, SNAP: 00 00 00, EtherType: 88 8E
        if (llc[0] != 0xAA || llc[1] != 0xAA || llc[2] != 0x03) return 0;
        if (llc[3] != 0x00 || llc[4] != 0x00 || llc[5] != 0x00) return 0;
        if (llc[6] != 0x88 || llc[7] != 0x8E) return 0; // EtherType EAPOL

        const uint8_t* eapol_data = llc + 8;
        int eapol_len = remain - l2_offset - 8;
        if (eapol_len < 4) return 0;

        return process_eapol_frame_(frame, mac, eapol_data, eapol_len);
    }

    // ---- 查询接口 ----

    int get_session_count() const { return session_count_; }

    const HandshakeSession* get_session(int index) const {
        if (index < 0 || index >= session_count_) return nullptr;
        return &sessions_[index];
    }

    int get_complete_handshakes() const {
        int count = 0;
        for (int i = 0; i < session_count_; ++i) {
            if (sessions_[i].complete) ++count;
        }
        return count;
    }

    int get_pmkid_captures() const {
        int count = 0;
        for (int i = 0; i < session_count_; ++i) {
            if (sessions_[i].pmkid_capture) ++count;
        }
        return count;
    }

    // ---- 导出 ----

    // 导出为 hashcat hc22000 格式（WPA-PMKID-22000）
    //   out_buf: 输出缓冲区
    //   max_len: 缓冲区大小
    //   返回: 写入字节数
    int export_hashcat_format(char* out_buf, int max_len) const {
        int pos = 0;
        auto app_char = [&](char c) { if (pos < max_len - 1) out_buf[pos++] = c; };
        auto app_hex = [&](uint8_t b) {
            char a = (b >> 4) & 0x0F;
            char d = b & 0x0F;
            app_char(a < 10 ? '0' + a : 'A' + a - 10);
            app_char(d < 10 ? '0' + d : 'A' + d - 10);
        };

        for (int i = 0; i < session_count_; ++i) {
            const HandshakeSession& s = sessions_[i];
            if (!s.complete && !s.pmkid_capture) continue;

            // WPA*PMKID*AP_MAC*STA_MAC*PMKID***
            // 或 WPA*02*MIC*AP_MAC*STA_MAC*ESSID***ANonce*EAPOL***
            if (s.complete) {
                app_char('W'); app_char('P'); app_char('A'); app_char('*');
                app_char('0'); app_char('2'); app_char('*');

                // MIC from M2
                for (int j = 0; j < 16; ++j) app_hex(s.mic_m2[j]);
                app_char('*');

                // AP MAC
                for (int j = 0; j < 6; ++j) app_hex(s.ap_mac[j]);
                app_char('*');

                // STA MAC
                for (int j = 0; j < 6; ++j) app_hex(s.sta_mac[j]);
                app_char('*');

                // ESSID (hex-encoded)
                for (int j = 0; j < 32 && s.ssid[j]; ++j) app_hex(static_cast<uint8_t>(s.ssid[j]));
                app_char('*');

                app_char('*'); app_char('*'); app_char('*');
                app_char('\n');
            }

            if (pos >= max_len - 200) break; // 防止溢出
        }

        out_buf[pos] = '\0';
        return pos;
    }

    // 清空会话
    void clear() {
        LockGuard lock(session_mutex_);
        session_count_ = 0;
    }

private:
    static constexpr int kMaxSessions = 32;

    HandshakeSession sessions_[kMaxSessions]{};
    int              session_count_ = 0;
    Mutex            session_mutex_;

    HandshakeCapture() = default;

    static const uint8_t* skip_radiotap_(const uint8_t* data, int len) {
        if (len < 8) return data;
        uint16_t radiotap_len = data[2] | (static_cast<uint16_t>(data[3]) << 8);
        if (radiotap_len < 8 || radiotap_len > static_cast<uint16_t>(len)) return data;
        return data + radiotap_len;
    }

    int process_eapol_frame_(const CapturedFrame& frame, const WifiMacHeader* mac,
                             const uint8_t* eapol, int len) {
        if (len < 4) return 0;

        uint8_t version = eapol[0];
        uint8_t pkt_type = eapol[1];
        (void)version;

        if (pkt_type != 3) return 0; // 仅处理 EAPOL-Key

        uint16_t body_len = eapol[2] | (static_cast<uint16_t>(eapol[3]) << 8);
        if (len < static_cast<int>(4 + body_len)) return 0;

        const uint8_t* key_data = eapol + 4;
        if (body_len < 95) return 0; // 最小 EAPOL-Key 长度

        // 解析 Key Info
        uint16_t key_info = key_data[1] | (static_cast<uint16_t>(key_data[2]) << 8);
        uint8_t  key_desc = key_data[3]; // 2=WPA, 254=WPA2
        bool is_pairwise = (key_info & KeyInfoBits::KeyType) != 0;
        bool has_key_ack = (key_info & KeyInfoBits::KeyAck) != 0;
        bool has_key_mic = (key_info & KeyInfoBits::KeyMic) != 0;
        bool has_install = (key_info & KeyInfoBits::Install) != 0;
        bool has_secure  = (key_info & KeyInfoBits::Secure) != 0;

        if (!is_pairwise) return 0; // 仅处理 Pairwise Key

        // 确定 STA 和 AP 的 MAC
        const uint8_t* sta = mac->addr2; // TA
        const uint8_t* ap  = mac->addr1; // RA
        // 若帧从 AP 发出则交换（from_ds=1）
        if (mac->frame_control.from_ds && !mac->frame_control.to_ds) {
            sta = mac->addr1;
            ap  = mac->addr2;
        }

        // 判定消息类型
        HandshakeMessage msg = classify_handshake_msg_(has_key_ack, has_key_mic,
                                                       has_install, has_secure);
        if (msg == HandshakeMessage::None) return 0;

        LockGuard lock(session_mutex_);

        // 查找或创建会话
        HandshakeSession* sess = find_or_create_session_(sta, ap);
        if (!sess) return 0;
        sess->last_seen_tick = get_tick_();
        sess->key_descriptor = key_desc;

        // 存储 EAPOL 帧数据
        store_eapol_frame_(sess, msg, eapol, len);

        // 提取 Nonce（偏移 17-48, 共 32 字节）
        if (body_len >= 49) {
            if (msg == HandshakeMessage::M1 && !sess->anonce_valid) {
                for (int i = 0; i < 32; ++i) sess->anonce[i] = key_data[17 + i];
                sess->anonce_valid = true;
            }
            if (msg == HandshakeMessage::M2 && !sess->snonce_valid) {
                for (int i = 0; i < 32; ++i) sess->snonce[i] = key_data[17 + i];
                sess->snonce_valid = true;
            }
        }

        // 提取 MIC（偏移 81-96, 共 16 字节）
        if (has_key_mic && body_len >= 97) {
            if (msg == HandshakeMessage::M2) {
                for (int i = 0; i < 16; ++i) sess->mic_m2[i] = key_data[81 + i];
            }
            if (msg == HandshakeMessage::M3) {
                for (int i = 0; i < 16; ++i) sess->mic_m3[i] = key_data[81 + i];
            }
        }

        // 尝试提取 PMKID（从 M1 的 Key Data 中的 RSN IE）
        if (msg == HandshakeMessage::M1) {
            extract_pmkid_(key_data, body_len, sess);
        }

        // 更新状态
        sess->last_message = msg;

        // 检查握手是否完整
        if (sess->eapol_m1_len > 0 && sess->eapol_m2_len > 0 &&
            sess->eapol_m3_len > 0 && sess->eapol_m4_len > 0) {
            sess->complete = true;
            return 2;
        }

        return 1;
    }

    static HandshakeMessage classify_handshake_msg_(bool key_ack, bool key_mic,
                                                     bool install, bool secure) {
        // M1: KeyAck=1, KeyMic=0, Install=0, Secure=0
        if (key_ack && !key_mic && !install && !secure) return HandshakeMessage::M1;
        // M2: KeyAck=0, KeyMic=1, Install=0, Secure=0
        if (!key_ack && key_mic && !install && !secure) return HandshakeMessage::M2;
        // M3: KeyAck=1, KeyMic=1, Install=1, Secure=0|1
        if (key_ack && key_mic && install) return HandshakeMessage::M3;
        // M4: KeyAck=0, KeyMic=1, Install=0, Secure=1
        if (!key_ack && key_mic && !install && secure) return HandshakeMessage::M4;
        return HandshakeMessage::None;
    }

    HandshakeSession* find_or_create_session_(const uint8_t* sta, const uint8_t* ap) {
        // 查找已存在会话
        for (int i = 0; i < session_count_; ++i) {
            if (mac_equal_(sessions_[i].sta_mac, sta) &&
                mac_equal_(sessions_[i].ap_mac, ap)) {
                return &sessions_[i];
            }
        }

        // 超时清理
        uint32_t now = get_tick_();
        int write = 0;
        for (int i = 0; i < session_count_; ++i) {
            if (now - sessions_[i].last_seen_tick < 60000 &&
                !sessions_[i].complete) { // 60s 超时
                if (write != i) sessions_[write] = sessions_[i];
                ++write;
            }
        }
        session_count_ = write;

        // 创建新会话
        if (session_count_ >= kMaxSessions) return nullptr;
        HandshakeSession& ns = sessions_[session_count_];
        for (int i = 0; i < 6; ++i) ns.sta_mac[i] = sta[i];
        for (int i = 0; i < 6; ++i) ns.ap_mac[i] = ap[i];
        ns.first_seen_tick = now;
        ns.last_seen_tick = now;
        ++session_count_;
        return &ns;
    }

    void store_eapol_frame_(HandshakeSession* sess, HandshakeMessage msg,
                            const uint8_t* eapol, int len) {
        uint8_t* dst = nullptr;
        uint16_t* dst_len = nullptr;
        int max_store = 0;

        switch (msg) {
            case HandshakeMessage::M1: dst = sess->eapol_m1; dst_len = &sess->eapol_m1_len; max_store = 256; break;
            case HandshakeMessage::M2: dst = sess->eapol_m2; dst_len = &sess->eapol_m2_len; max_store = 256; break;
            case HandshakeMessage::M3: dst = sess->eapol_m3; dst_len = &sess->eapol_m3_len; max_store = 256; break;
            case HandshakeMessage::M4: dst = sess->eapol_m4; dst_len = &sess->eapol_m4_len; max_store = 256; break;
            default: return;
        }

        int copy_len = (len < max_store) ? len : max_store;
        for (int i = 0; i < copy_len; ++i) dst[i] = eapol[i];
        *dst_len = static_cast<uint16_t>(copy_len);
    }

    void extract_pmkid_(const uint8_t* key_data, int body_len, HandshakeSession* sess) {
        // Key Data 长度在偏移 97-98
        if (body_len < 99) return;
        uint16_t kd_len = key_data[97] | (static_cast<uint16_t>(key_data[98]) << 8);
        if (kd_len == 0) return;

        const uint8_t* kd = key_data + 99; // Key Data 起始（偏移 95 是 MIC 结束）
        int kd_available = body_len - 99;
        if (kd_available < static_cast<int>(kd_len)) return;

        // 在 Key Data 中查找 RSN IE（tag=0x30）
        int pos = 0;
        while (pos + 2 <= kd_len) {
            uint8_t tag = kd[pos];
            uint8_t ie_len = kd[pos + 1];
            if (pos + 2 + ie_len > kd_len) break;

            if (tag == 0x30 && ie_len >= 22) {
                // RSN IE → PMKID Count 在偏移 14（2 bytes）
                uint16_t pmkid_count = kd[pos + 14] | (static_cast<uint16_t>(kd[pos + 15]) << 8);
                if (pmkid_count > 0 && ie_len >= 22 + static_cast<int>(pmkid_count) * 16) {
                    // 第一个 PMKID 在偏移 16，长度 16
                    for (int i = 0; i < 16; ++i) {
                        sess->pmkid[i] = kd[pos + 16 + i];
                    }
                    sess->pmkid_valid = true;
                    sess->pmkid_capture = true;
                }
                break;
            }
            pos += 2 + ie_len;
        }
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

#endif // AURORA_WIRELESS_HANDSHAKE_CAPTURE_HPP
