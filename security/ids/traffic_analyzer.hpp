// =============================================================================
// security/ids/traffic_analyzer.hpp
//
// NIDS 流量行为分析：包解析 + 每主机流量统计 + 基线
//
//   - parse_packet()：零分配的以太网/IPv4/TCP/UDP/ARP 防御性解析，
//     输出 ParsedPacket 与畸形标记（供协议异常检测）
//   - TrafficAnalyzer：每主机当前窗口计数（包数/字节数/SYN/去重端口/去重目标 IP），
//     跨窗口维护包速率 EMA 基线，tick() 滚动窗口并识别离群点
//
// 设计原则（遵循 AGENTS.md）：固定数组、全定点、零堆分配、noexcept
// =============================================================================
#ifndef AURORA_IDS_TRAFFIC_ANALYZER_HPP
#define AURORA_IDS_TRAFFIC_ANALYZER_HPP

#include <stdint.h>
#include "alert_manager.hpp"

namespace aurora {
namespace ids {

// ---------------------------------------------------------------------------
// 解析后的包
// ---------------------------------------------------------------------------
struct ParsedPacket {
    uint16_t eth_type = 0;
    bool is_ipv4 = false;
    bool is_arp = false;

    // IPv4
    uint8_t ip_proto = 0;
    uint8_t ihl = 0;
    uint32_t src_ip = 0;
    uint32_t dst_ip = 0;
    uint16_t frag_off = 0;
    bool more_frag = false;
    uint16_t total_len = 0;

    // L4
    uint16_t src_port = 0;
    uint16_t dst_port = 0;
    uint8_t tcp_flags = 0;
    uint8_t tcp_data_off = 0;
    const uint8_t* payload = nullptr;
    int payload_len = 0;

    // ARP
    uint16_t arp_op = 0;
    uint32_t arp_spa = 0;
    const uint8_t* arp_sha = nullptr;

    // 畸形标记
    bool malformed = false;
    const char* malformed_reason = nullptr;
};

static inline uint16_t ids_read16be(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

static inline uint32_t ids_read32be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// 解析以太网帧（IPv4 / ARP）。len < 14 时返回 false，其余返回 true 并置 malformed 标记。
static inline bool parse_packet(const uint8_t* pkt, int len, ParsedPacket& out) {
    out = ParsedPacket{};
    if (len < 14) {
        out.malformed = true;
        out.malformed_reason = "frame too short";
        return false;
    }
    out.eth_type = ids_read16be(pkt + 12);

    // ---- ARP ----
    if (out.eth_type == 0x0806) {
        out.is_arp = true;
        if (len < 42) {
            out.malformed = true;
            out.malformed_reason = "ARP too short";
            return true;
        }
        out.arp_op = ids_read16be(pkt + 20);
        out.arp_sha = pkt + 22;
        out.arp_spa = ids_read32be(pkt + 28);
        return true;
    }

    // ---- 非 IPv4 ----
    if (out.eth_type != 0x0800)
        return true;

    out.is_ipv4 = true;
    if (len < 34) {
        out.malformed = true;
        out.malformed_reason = "IP header too short";
        return true;
    }

    out.ihl = pkt[14] & 0x0F;
    if (out.ihl < 5) {
        out.malformed = true;
        out.malformed_reason = "IHL < 5";
        return true;
    }
    out.total_len = ids_read16be(pkt + 16);
    out.ip_proto = pkt[23];
    out.src_ip = ids_read32be(pkt + 26);
    out.dst_ip = ids_read32be(pkt + 30);
    const uint16_t frag = ids_read16be(pkt + 20);
    out.frag_off = frag & 0x1FFFu;
    out.more_frag = (frag & 0x2000u) != 0;

    if (out.total_len < static_cast<uint16_t>(out.ihl * 4)) {
        out.malformed = true;
        out.malformed_reason = "total_len < IP header";
        return true;
    }
    if (out.total_len != 0 && (14 + out.total_len) > len) {
        out.malformed = true;
        out.malformed_reason = "total_len exceeds frame";
        return true;
    }

    const int l4_off = 14 + out.ihl * 4;

    if (out.ip_proto == 6) { // TCP
        if (len < l4_off + 20) {
            out.malformed = true;
            out.malformed_reason = "TCP header too short";
            return true;
        }
        out.src_port = ids_read16be(pkt + l4_off);
        out.dst_port = ids_read16be(pkt + l4_off + 2);
        out.tcp_data_off = (pkt[l4_off + 12] >> 4) & 0x0F;
        out.tcp_flags = pkt[l4_off + 13];
        if (out.tcp_data_off < 5) {
            out.malformed = true;
            out.malformed_reason = "TCP data offset < 5";
            return true;
        }
        const int tcp_hdr_sz = out.tcp_data_off * 4;
        out.payload = pkt + l4_off + tcp_hdr_sz;
        out.payload_len = len - (l4_off + tcp_hdr_sz);
        if (out.payload_len < 0)
            out.payload_len = 0;
    } else if (out.ip_proto == 17) { // UDP
        if (len < l4_off + 8) {
            out.malformed = true;
            out.malformed_reason = "UDP header too short";
            return true;
        }
        out.src_port = ids_read16be(pkt + l4_off);
        out.dst_port = ids_read16be(pkt + l4_off + 2);
        out.payload = pkt + l4_off + 8;
        out.payload_len = len - (l4_off + 8);
    } else {
        out.payload = pkt + l4_off;
        out.payload_len = len - l4_off;
    }

    return true;
}

// ---------------------------------------------------------------------------
// 每主机流量统计
// ---------------------------------------------------------------------------
struct HostFlow {
    bool active = false;
    uint32_t src_ip = 0;
    uint32_t pkt_count = 0;
    uint32_t byte_count = 0;
    uint32_t syn_count = 0;
    uint16_t dst_ports[16];
    uint8_t port_count = 0;
    uint32_t dst_ips[8];
    uint8_t dst_ip_count = 0;
    uint32_t window_start_ms = 0;
    uint32_t baseline_q8 = 0; // 每窗口包数的 EMA（Q8）
};

class TrafficAnalyzer {
public:
    static constexpr int kMaxHosts = 16;
    static constexpr uint32_t kWindowMs = 1000;

    // 更新 src 的当前窗口统计，返回对应 flow（槽满返回 nullptr）。
    HostFlow* process_packet(const ParsedPacket& p, int frame_len) {
        const uint32_t now = now_ms_();
        HostFlow* f = get_or_create_(p.src_ip, now);
        if (!f)
            return nullptr;

        ++f->pkt_count;
        f->byte_count += static_cast<uint32_t>(frame_len > 0 ? frame_len : 0);

        if (p.ip_proto == 6 && (p.tcp_flags & 0x02)) {
            ++f->syn_count;
            record_port_(f, p.dst_port);
        }
        record_host_(f, p.dst_ip);
        return f;
    }

    // 滚动所有过期窗口、更新基线。返回检测到的离群点主机数。
    int tick() {
        const uint32_t now = now_ms_();
        int outliers = 0;
        for (int i = 0; i < kMaxHosts; ++i) {
            if (flows_[i].active && (now - flows_[i].window_start_ms) >= kWindowMs) {
                if (roll_flow_(flows_[i], now))
                    ++outliers;
            }
        }
        return outliers;
    }

    HostFlow* get_flow(int i) {
        return (i >= 0 && i < kMaxHosts) ? &flows_[i] : nullptr;
    }

    int get_active_count() const {
        int n = 0;
        for (int i = 0; i < kMaxHosts; ++i)
            if (flows_[i].active)
                ++n;
        return n;
    }

    void reset() {
        for (int i = 0; i < kMaxHosts; ++i)
            flows_[i] = HostFlow{};
    }

private:
    HostFlow flows_[kMaxHosts]{};

    static uint32_t now_ms_() {
        return tick_count; // 全局声明见 alert_manager.hpp
    }

    HostFlow* get_or_create_(uint32_t src_ip, uint32_t now) {
        for (int i = 0; i < kMaxHosts; ++i) {
            if (flows_[i].active && flows_[i].src_ip == src_ip)
                return &flows_[i];
        }
        for (int i = 0; i < kMaxHosts; ++i) {
            if (!flows_[i].active) {
                flows_[i] = HostFlow{};
                flows_[i].active = true;
                flows_[i].src_ip = src_ip;
                flows_[i].window_start_ms = now;
                return &flows_[i];
            }
        }
        return nullptr;
    }

    void record_port_(HostFlow* f, uint16_t port) {
        for (uint8_t i = 0; i < f->port_count; ++i)
            if (f->dst_ports[i] == port)
                return;
        if (f->port_count < 16)
            f->dst_ports[f->port_count++] = port;
    }

    void record_host_(HostFlow* f, uint32_t ip) {
        for (uint8_t i = 0; i < f->dst_ip_count; ++i)
            if (f->dst_ips[i] == ip)
                return;
        if (f->dst_ip_count < 8)
            f->dst_ips[f->dst_ip_count++] = ip;
    }

    // 滚动单个窗口：离群点判定 + 基线 EMA 更新 + 计数清零
    bool roll_flow_(HostFlow& f, uint32_t now) {
        bool outlier = false;
        if (f.baseline_q8 > 0) {
            const uint32_t rate_q8 = f.pkt_count * 256u;
            if (rate_q8 > (f.baseline_q8 * 2u) + (20u * 256u))
                outlier = true;
        }
        // 基线 EMA（alpha = 1/4）
        f.baseline_q8 = f.baseline_q8 + ((f.pkt_count * 256u) - f.baseline_q8) / 4u;

        f.pkt_count = 0;
        f.byte_count = 0;
        f.syn_count = 0;
        f.port_count = 0;
        f.dst_ip_count = 0;
        f.window_start_ms = now;
        return outlier;
    }
};

} // namespace ids
} // namespace aurora

#endif // AURORA_IDS_TRAFFIC_ANALYZER_HPP
