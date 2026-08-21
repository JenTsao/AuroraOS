// =============================================================================
// security/ids/anomaly_detector.hpp
//
// NIDS 统计异常与协议异常检测
//
// 组合 TrafficAnalyzer + SignatureDb + AlertManager，逐包执行：
//   - 协议异常：畸形包（IHL/数据偏移/长度）、IP 分片、TCP 标志位扫描（NULL/FIN/XMAS）
//   - 扫描与洪水：端口扫描、主机扫描、SYN Flood（基于每主机窗口计数）
//   - ARP 欺骗：同一 IP 被不同 MAC 声明
//   - DNS 隧道：超长查询名、过多标签、TXT/NULL 记录、查询洪水
//   - 载荷特征：Aho-Corasick 多模式匹配
//   - 统计离群：包速率相对基线的离群点（tick() 中窗口滚动时判定）
//
// 设计原则（遵循 AGENTS.md）：组合优于继承、固定数组、零堆分配
// =============================================================================
#ifndef AURORA_IDS_ANOMALY_DETECTOR_HPP
#define AURORA_IDS_ANOMALY_DETECTOR_HPP

#include <stdint.h>
#include "alert_manager.hpp"
#include "signature_db.hpp"
#include "traffic_analyzer.hpp"

namespace aurora {
namespace ids {

class AnomalyDetector {
public:
    AnomalyDetector(TrafficAnalyzer& traffic, SignatureDb& sigdb, AlertManager& alerts) noexcept
        : traffic_(traffic), sigdb_(sigdb), alerts_(alerts) {}

    // ---- 阈值配置 ----
    void set_port_scan_threshold(uint8_t n) noexcept { port_scan_threshold_ = n; }
    void set_host_scan_threshold(uint8_t n) noexcept { host_scan_threshold_ = n; }
    void set_syn_flood_threshold(uint32_t n) noexcept { syn_flood_threshold_ = n; }
    void set_dns_query_threshold(uint32_t n) noexcept { dns_query_threshold_ = n; }

    // ---- 逐包检测 ----
    void process_packet(const uint8_t* pkt, int len) {
        ParsedPacket p;
        if (!parse_packet(pkt, len, p))
            return;

        if (p.malformed) {
            alerts_.submit(IdsCategory::MalformedPacket, IdsSeverity::Medium,
                           p.src_ip, p.dst_ip, p.src_port, p.dst_port, p.malformed_reason);
        }

        if (p.is_arp) {
            check_arp_(p);
            return;
        }
        if (!p.is_ipv4)
            return;

        // IP 分片
        if (p.frag_off != 0 || p.more_frag) {
            alerts_.submit(IdsCategory::Fragmentation, IdsSeverity::Low,
                           p.src_ip, p.dst_ip, 0, 0, "IP fragmentation");
        }

        // TCP 标志位异常扫描
        if (p.ip_proto == 6 && !p.malformed) {
            check_tcp_flags_(p);
        }

        // 每主机流量统计 + 扫描/洪水检测
        HostFlow* f = traffic_.process_packet(p, len);
        if (f) {
            check_scan_(p, *f);
        }

        // DNS 隧道（UDP 53）
        if (p.ip_proto == 17 && (p.dst_port == 53 || p.src_port == 53)) {
            check_dns_(p);
        }

        // 载荷特征
        if (p.payload_len > 0) {
            const int sid = sigdb_.match(p.payload, p.payload_len);
            if (sid >= 0) {
                const SignatureRule& r = sigdb_.rule(sid);
                alerts_.submit(IdsCategory::PayloadSignature, r.severity,
                               p.src_ip, p.dst_ip, p.src_port, p.dst_port, r.message);
            }
        }
    }

    // ---- 周期调用：窗口滚动、基线离群点、DNS 查询窗口重置 ----
    void tick() {
        const int outliers = traffic_.tick();
        if (outliers > 0) {
            alerts_.submit(IdsCategory::TrafficAnomaly, IdsSeverity::Medium,
                           0, 0, 0, 0, "traffic rate outlier");
        }

        const uint32_t now = now_ms_();
        for (int i = 0; i < kMaxDnsHosts; ++i) {
            if (dns_hosts_[i].active && (now - dns_hosts_[i].window_start_ms) >= kDnsWindowMs) {
                dns_hosts_[i].query_count = 0;
                dns_hosts_[i].window_start_ms = now;
            }
        }
    }

    void reset() noexcept {
        traffic_.reset();
        for (int i = 0; i < kMaxArpEntries; ++i)
            arp_table_[i] = ArpEntry{};
        for (int i = 0; i < kMaxDnsHosts; ++i)
            dns_hosts_[i] = DnsHost{};
    }

private:
    static constexpr int kMaxArpEntries = 16;
    static constexpr int kMaxDnsHosts = 8;
    static constexpr uint32_t kDnsWindowMs = 1000;
    static constexpr uint32_t kMaxDnsNameLen = 52;
    static constexpr uint8_t kMaxDnsLabels = 6;

    struct ArpEntry {
        bool active = false;
        uint32_t ip = 0;
        uint8_t mac[6] = {0};
    };
    struct DnsHost {
        bool active = false;
        uint32_t ip = 0;
        uint32_t query_count = 0;
        uint32_t window_start_ms = 0;
    };

    TrafficAnalyzer& traffic_;
    SignatureDb& sigdb_;
    AlertManager& alerts_;

    ArpEntry arp_table_[kMaxArpEntries]{};
    DnsHost dns_hosts_[kMaxDnsHosts]{};

    uint8_t port_scan_threshold_ = 10;
    uint8_t host_scan_threshold_ = 5;
    uint32_t syn_flood_threshold_ = 50;
    uint32_t dns_query_threshold_ = 20;

    static uint32_t now_ms_() noexcept {
        return tick_count; // 全局声明见 alert_manager.hpp
    }

    // ---- ARP 欺骗：同一 IP 被不同 MAC 声明 ----
    void check_arp_(const ParsedPacket& p) {
        if (p.arp_op != 2 || !p.arp_sha)
            return; // 仅关注 ARP 应答

        for (int i = 0; i < kMaxArpEntries; ++i) {
            ArpEntry& e = arp_table_[i];
            if (e.active && e.ip == p.arp_spa) {
                bool same = true;
                for (int k = 0; k < 6; ++k) {
                    if (e.mac[k] != p.arp_sha[k]) {
                        same = false;
                        break;
                    }
                }
                if (!same) {
                    alerts_.submit(IdsCategory::ArpSpoof, IdsSeverity::High,
                                   p.arp_spa, 0, 0, 0, "ARP spoofing (MAC change)");
                    for (int k = 0; k < 6; ++k)
                        e.mac[k] = p.arp_sha[k];
                }
                return;
            }
        }

        for (int i = 0; i < kMaxArpEntries; ++i) {
            if (!arp_table_[i].active) {
                arp_table_[i].active = true;
                arp_table_[i].ip = p.arp_spa;
                for (int k = 0; k < 6; ++k)
                    arp_table_[i].mac[k] = p.arp_sha[k];
                return;
            }
        }
    }

    // ---- TCP 标志位异常（NULL / XMAS / FIN 扫描） ----
    void check_tcp_flags_(const ParsedPacket& p) {
        const uint8_t f = p.tcp_flags;
        const bool syn = (f & 0x02) != 0;
        const bool fin = (f & 0x01) != 0;
        const bool psh = (f & 0x08) != 0;
        const bool urg = (f & 0x20) != 0;
        const bool ack = (f & 0x10) != 0;

        if ((f & 0x3F) == 0) {
            alerts_.submit(IdsCategory::TcpFlagAnomaly, IdsSeverity::Medium,
                           p.src_ip, p.dst_ip, p.src_port, p.dst_port, "NULL scan");
        } else if (fin && psh && urg) {
            alerts_.submit(IdsCategory::TcpFlagAnomaly, IdsSeverity::Medium,
                           p.src_ip, p.dst_ip, p.src_port, p.dst_port, "XMAS scan");
        } else if (fin && !ack && !syn) {
            alerts_.submit(IdsCategory::TcpFlagAnomaly, IdsSeverity::Medium,
                           p.src_ip, p.dst_ip, p.src_port, p.dst_port, "FIN scan");
        }
    }

    // ---- 端口扫描 / 主机扫描 / SYN 洪水 ----
    void check_scan_(const ParsedPacket& p, HostFlow& f) {
        if (f.port_count > port_scan_threshold_) {
            alerts_.submit(IdsCategory::PortScan, IdsSeverity::High,
                           p.src_ip, 0, 0, 0, "port scan");
        }
        if (f.dst_ip_count > host_scan_threshold_) {
            alerts_.submit(IdsCategory::HostScan, IdsSeverity::High,
                           p.src_ip, 0, 0, 0, "host scan");
        }
        if (f.syn_count > syn_flood_threshold_) {
            alerts_.submit(IdsCategory::SynFlood, IdsSeverity::Critical,
                           p.src_ip, 0, 0, 0, "SYN flood");
        }
    }

    // ---- DNS 隧道（UDP 53） ----
    void check_dns_(const ParsedPacket& p) {
        const uint8_t* dns = p.payload;
        const int dlen = p.payload_len;
        if (dlen < 12)
            return;

        // 仅关注查询（QR = 0）
        if ((dns[2] & 0x80) != 0)
            return;

        // 解析首个问题名
        int pos = 12;
        int name_len = 0;
        int labels = 0;
        while (pos < dlen) {
            const uint8_t l = dns[pos];
            if (l == 0) {
                ++pos;
                break;
            }
            if ((l & 0xC0) == 0xC0) {
                pos += 2;
                break;
            }
            ++labels;
            name_len += l + 1;
            pos += l + 1;
            if (name_len > 255)
                break;
        }
        if (pos + 2 > dlen)
            return;
        const uint16_t qtype = ids_read16be(dns + pos);

        // 查询洪水（每主机窗口计数）
        uint32_t* qc = dns_query_count_(p.src_ip);
        if (qc) {
            ++(*qc);
            if (*qc > dns_query_threshold_) {
                alerts_.submit(IdsCategory::DnsTunnel, IdsSeverity::High,
                               p.src_ip, p.dst_ip, p.src_port, p.dst_port, "DNS query flood");
            }
        }

        // 隧道特征：超长名 / 过多标签 / TXT/NULL 记录
        if (name_len > static_cast<int>(kMaxDnsNameLen)) {
            alerts_.submit(IdsCategory::DnsTunnel, IdsSeverity::Medium,
                           p.src_ip, p.dst_ip, p.src_port, p.dst_port, "long DNS name");
        } else if (labels > kMaxDnsLabels) {
            alerts_.submit(IdsCategory::DnsTunnel, IdsSeverity::Low,
                           p.src_ip, p.dst_ip, p.src_port, p.dst_port, "many DNS labels");
        } else if (qtype == 16 || qtype == 10) { // TXT / NULL
            alerts_.submit(IdsCategory::DnsTunnel, IdsSeverity::Low,
                           p.src_ip, p.dst_ip, p.src_port, p.dst_port, "TXT/NULL record");
        }
    }

    uint32_t* dns_query_count_(uint32_t ip) {
        for (int i = 0; i < kMaxDnsHosts; ++i) {
            if (dns_hosts_[i].active && dns_hosts_[i].ip == ip)
                return &dns_hosts_[i].query_count;
        }
        for (int i = 0; i < kMaxDnsHosts; ++i) {
            if (!dns_hosts_[i].active) {
                dns_hosts_[i].active = true;
                dns_hosts_[i].ip = ip;
                dns_hosts_[i].query_count = 0;
                dns_hosts_[i].window_start_ms = now_ms_();
                return &dns_hosts_[i].query_count;
            }
        }
        return nullptr;
    }
};

} // namespace ids
} // namespace aurora

#endif // AURORA_IDS_ANOMALY_DETECTOR_HPP
