#ifndef PROTOCOL_ANALYZER_HPP
#define PROTOCOL_ANALYZER_HPP

#include <stdint.h>
#include "packet_capture.hpp"

// ============================================================
// ProtocolAnalyzer — BPF 风格包过滤引擎
//
// 在 IRQ 上下文或高优先级任务中调用，因此：
//   - 无动态分配
//   - 无虚函数
//   - 全静态内联，编译器可充分优化
//   - 对不完整/畸形包做防御性边界检查
// ============================================================

class ProtocolAnalyzer {
public:
    // ---- 主入口 ----
    // 返回 true = 放行此包，false = 丢弃
    static bool match_filter(const uint8_t* pkt, int len, const BpfFilter& f) {
        // 解析以太网头 (14 字节)
        if (len < 14) return !any_enabled_(f);   // 太短无法解析 → 无过滤规则则放行

        uint16_t eth_type = (pkt[12] << 8) | pkt[13];

        // 收集各层匹配结果
        bool l2_pass = true;   // MAC 层
        bool l3_pass = true;   // IP 层
        bool l4_pass = true;   // 传输层

        // ---- L2: MAC 过滤 ----
        l2_pass = match_mac_(pkt, f);

        // ---- L3: IP / 协议过滤 ----
        if (eth_type != 0x0800) {
            // 非 IPv4：如果有 IP 层过滤规则则不通过
            if (f.ip_src.enabled || f.ip_dst.enabled ||
                f.proto_enabled || f.port_src.enabled ||
                f.port_dst.enabled || f.tcp_flags_enabled) {
                l3_pass = false;
            }
        } else {
            // IPv4 包
            if (len < 34) return !any_enabled_(f);  // IP 头不完整
            l3_pass = match_ip_(pkt, f);

            // L4: 传输层过滤
            uint8_t  ip_proto   = pkt[23];
            uint8_t  ihl        = pkt[14] & 0x0F;
            if (ihl < 5) { l4_pass = false; }  // 畸形 IHL
            else {
                int ip_hdr_sz = ihl * 4;
                l4_pass = match_transport_(pkt, len, ip_proto, ip_hdr_sz, f);
            }
        }

        // ---- 复合判定 (AND / OR) ----
        return compound_eval_(l2_pass, l3_pass, l4_pass, f);
    }

private:
    // ---- 辅助 ----

    // 是否有任意过滤规则启用
    static bool any_enabled_(const BpfFilter& f) {
        return f.mac_src.enabled  || f.mac_dst.enabled  ||
               f.ip_src.enabled   || f.ip_dst.enabled   ||
               f.proto_enabled    || f.port_src.enabled ||
               f.port_dst.enabled || f.tcp_flags_enabled;
    }

    // AND / OR 复合
    static bool compound_eval_(bool mac, bool ip, bool l4, const BpfFilter& f) {
        if (f.filter_or) {
            // OR 模式：任一启用的维度通过即放行
            bool any_active = false;
            if (f.mac_src.enabled  || f.mac_dst.enabled)  { if (mac) return true; any_active = true; }
            if (f.ip_src.enabled   || f.ip_dst.enabled   || f.proto_enabled) { if (ip)  return true; any_active = true; }
            if (f.port_src.enabled || f.port_dst.enabled  || f.tcp_flags_enabled) { if (l4)  return true; any_active = true; }
            return !any_active;   // 无任何规则启用 → 放行
        } else {
            // AND 模式：全部启用的维度必须通过
            if (f.mac_src.enabled  || f.mac_dst.enabled)  { if (!mac) return false; }
            if (f.ip_src.enabled   || f.ip_dst.enabled   || f.proto_enabled) { if (!ip)  return false; }
            if (f.port_src.enabled || f.port_dst.enabled  || f.tcp_flags_enabled) { if (!l4)  return false; }
            return true;
        }
    }

    // ---- MAC 匹配 ----
    static bool match_mac_(const uint8_t* pkt, const BpfFilter& f) {
        if (!f.mac_src.enabled && !f.mac_dst.enabled) return true;

        bool src_ok = !f.mac_src.enabled;
        bool dst_ok = !f.mac_dst.enabled;

        // 以太网帧: [0..5]=dst MAC, [6..11]=src MAC
        if (f.mac_src.enabled) {
            src_ok = true;
            for (int i = 0; i < 6; ++i)
                if (pkt[6 + i] != f.mac_src.addr[i]) { src_ok = false; break; }
        }
        if (f.mac_dst.enabled) {
            dst_ok = (pkt[0] == 0xFF && pkt[1] == 0xFF && pkt[2] == 0xFF &&
                      pkt[3] == 0xFF && pkt[4] == 0xFF && pkt[5] == 0xFF);
            // broadcast 总是匹配 — 否则逐字节对比
            if (!dst_ok) {
                dst_ok = true;
                for (int i = 0; i < 6; ++i)
                    if (pkt[i] != f.mac_dst.addr[i]) { dst_ok = false; break; }
            }
        }
        return src_ok && dst_ok;
    }

    // ---- IP 匹配 (含 CIDR 掩码) ----
    static bool match_ip_(const uint8_t* pkt, const BpfFilter& f) {
        if (!f.ip_src.enabled && !f.ip_dst.enabled && !f.proto_enabled) return true;

        // IPv4: src IP = offset 26, dst IP = offset 30  (大端序)
        uint32_t ip_src = read32be_(pkt + 26);
        uint32_t ip_dst = read32be_(pkt + 30);

        bool src_ok = !f.ip_src.enabled;
        bool dst_ok = !f.ip_dst.enabled;
        bool proto_ok = !f.proto_enabled;

        if (f.ip_src.enabled) {
            uint32_t mask = (f.ip_src.mask == 0) ? 0xFFFFFFFF : f.ip_src.mask;
            src_ok = ((ip_src ^ f.ip_src.addr) & mask) == 0;
        }
        if (f.ip_dst.enabled) {
            uint32_t mask = (f.ip_dst.mask == 0) ? 0xFFFFFFFF : f.ip_dst.mask;
            dst_ok = ((ip_dst ^ f.ip_dst.addr) & mask) == 0;
        }
        if (f.proto_enabled) {
            uint8_t proto = pkt[23];
            if (f.proto_bitmask == 0) {
                proto_ok = false;  // 位图=0 → 不匹配任何协议
            } else {
                proto_ok = (proto < 32) && (f.proto_bitmask & (1u << proto));
            }
        }
        return src_ok && dst_ok && proto_ok;
    }

    // ---- 传输层匹配 (端口范围 + TCP flags) ----
    static bool match_transport_(const uint8_t* pkt, int len,
                                  uint8_t proto, int ip_hdr_sz,
                                  const BpfFilter& f) {
        if (!f.port_src.enabled && !f.port_dst.enabled && !f.tcp_flags_enabled)
            return true;

        // 仅 TCP(6) / UDP(17) 有端口概念
        if (proto != 6 && proto != 17) return false;

        int l4_off = 14 + ip_hdr_sz;
        if (len < l4_off + 4) return false;   // L4 头不完整

        uint16_t sport = read16be_(pkt + l4_off);
        uint16_t dport = read16be_(pkt + l4_off + 2);

        bool src_ok = !f.port_src.enabled;
        bool dst_ok = !f.port_dst.enabled;
        bool flg_ok = !f.tcp_flags_enabled;

        if (f.port_src.enabled)
            src_ok = in_range_(sport, f.port_src.lo, f.port_src.hi);
        if (f.port_dst.enabled)
            dst_ok = in_range_(dport, f.port_dst.lo, f.port_dst.hi);

        // TCP flags (仅 proto == 6 有意义)
        if (f.tcp_flags_enabled && proto == 6) {
            if (len < l4_off + 14) return false;       // TCP 头不完整
            uint8_t fl  = pkt[l4_off + 13];            // TCP flags 字段
            flg_ok = (fl & f.tcp_flags_val) == f.tcp_flags_val;
        } else if (f.tcp_flags_enabled) {
            flg_ok = false;                            // 非 TCP 包 → 不匹配
        }

        return src_ok && dst_ok && flg_ok;
    }

    // ---- 工具函数 ----

    static inline uint16_t read16be_(const uint8_t* p) {
        return (static_cast<uint16_t>(p[0]) << 8) | p[1];
    }

    static inline uint32_t read32be_(const uint8_t* p) {
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) <<  8) |
                static_cast<uint32_t>(p[3]);
    }

    static inline bool in_range_(uint16_t val, uint16_t lo, uint16_t hi) {
        // 网络字节序：直接比较（lo ≤ val ≤ hi 在大端序下含义正确）
        if (lo <= hi) return val >= lo && val <= hi;
        return val >= lo || val <= hi;  // 环绕范围（如 lo=65535, hi=5 → port 0-5 | 65535）
    }
};

#endif // PROTOCOL_ANALYZER_HPP
