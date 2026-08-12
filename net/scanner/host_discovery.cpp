// ============================================================
// host_discovery.cpp -- ARP 扫描 / ICMP Ping 主机发现引擎实现
//
// 将 header-only 实现中的 ARP/ICMP 网络 I/O 方法提取到 .cpp。
// ============================================================

#include "host_discovery.hpp"
#include <stdint.h>
#include <string.h>

extern "C" {
#include "net_client.hpp"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/icmp.h"
#include "lwip/ip4.h"
#include "lwip/inet.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "lwip/raw.h"
}

// ---- 系统全局符号 ----
extern volatile uint32_t tick_count;
extern void sys_yield();

// ============================================================
// 工具函数
// ============================================================

uint32_t HostDiscovery::get_tick_count_() {
    return tick_count;
}

void HostDiscovery::yield_cpu_() {
    sys_yield();
}

// ============================================================
// ARP 扫描 -- 单个 IP
// ============================================================

HostResult HostDiscovery::arp_scan(uint32_t target_ip) {
    LockGuard lock(scan_mutex_);

    HostResult result{};
    result.ip = target_ip;
    result.scheduler.state = HostState::Down;
    result.mac_resolved = false;

    if (!netif_) {
        result.scheduler.state = HostState::Unknown;
        return result;
    }

    pending_ip_ = target_ip;
    arp_reply_received_ = false;

    for (uint8_t r = 0; r < retries_; ++r) {
        send_arp_request_(target_ip);

        uint32_t start = get_tick_count_();
        while ((get_tick_count_() - start) < timeout_ms_) {
            yield_cpu_();

            // 检查 lwIP ARP 表
            err_t arp_err = etharp_query(netif_,
                reinterpret_cast<const ip4_addr_t*>(&target_ip), nullptr);
            if (arp_err == ERR_OK) {
                struct eth_addr* eth_ret = nullptr;
                const ip4_addr_t ip;
                ip4_addr_set_u32(const_cast<ip4_addr_t*>(&ip), target_ip);
                err_t find_err = etharp_find_addr(netif_, &ip, &eth_ret, nullptr);
                if (find_err == ERR_OK && eth_ret) {
                    for (int i = 0; i < MAC_ADDR_LEN; ++i) {
                        result.mac[i] = eth_ret->addr[i];
                    }
                    result.scheduler.state = HostState::Up;
                    result.mac_resolved = true;
                    result.latency_ms = get_tick_count_() - start;
                    return result;
                }
            }

            if (arp_reply_received_) {
                result.scheduler.state = HostState::Up;
                result.mac_resolved = true;
                result.latency_ms = get_tick_count_() - start;
                const ip4_addr_t ip;
                ip4_addr_set_u32(const_cast<ip4_addr_t*>(&ip), target_ip);
                struct eth_addr* eth_ret = nullptr;
                err_t find_err = etharp_find_addr(netif_, &ip, &eth_ret, nullptr);
                if (find_err == ERR_OK && eth_ret) {
                    for (int i = 0; i < MAC_ADDR_LEN; ++i) {
                        result.mac[i] = eth_ret->addr[i];
                    }
                }
                return result;
            }
        }
    }

    result.scheduler.state = HostState::Down;
    result.latency_ms = timeout_ms_ * retries_;
    return result;
}

// ============================================================
// ARP 扫描 -- 整个 /24 子网
// ============================================================

int HostDiscovery::arp_scan_subnet(uint32_t network_prefix,
                                    HostResult* out_results, int max_results) {
    uint32_t base = network_prefix & 0xFFFFFF00;
    int alive_count = 0;

    for (uint16_t host = 1; host <= 254 && alive_count < max_results; ++host) {
        uint32_t target_ip = base | htonl(host);
        HostResult result = arp_scan(target_ip);

        if (result.scheduler.state == HostState::Up) {
            out_results[alive_count] = result;
            ++alive_count;
        }

        yield_cpu_();
    }

    return alive_count;
}

// ============================================================
// ICMP Ping -- 单个 IP
// ============================================================

HostResult HostDiscovery::icmp_ping(uint32_t target_ip) {
    LockGuard lock(scan_mutex_);

    HostResult result{};
    result.ip = target_ip;
    result.scheduler.state = HostState::Down;
    result.mac_resolved = false;

    pending_icmp_ip_ = target_ip;
    icmp_reply_received_ = false;

    struct raw_pcb* pcb = raw_new(IP_PROTO_ICMP);
    if (!pcb) {
        result.scheduler.state = HostState::Unknown;
        return result;
    }

    raw_recv(pcb, icmp_recv_callback_);
    raw_bind(pcb, IP_ADDR_ANY);
    pcb->recv_arg = this;

    for (uint8_t r = 0; r < retries_; ++r) {
        send_icmp_echo_(pcb, target_ip, static_cast<uint16_t>(icmp_seq_));
        ++icmp_seq_;

        uint32_t start = get_tick_count_();
        while ((get_tick_count_() - start) < timeout_ms_) {
            yield_cpu_();

            if (icmp_reply_received_ && pending_icmp_ip_ == target_ip) {
                result.scheduler.state = HostState::Up;
                result.latency_ms = get_tick_count_() - start;
                raw_remove(pcb);
                return result;
            }
        }
    }

    raw_remove(pcb);
    result.latency_ms = timeout_ms_ * retries_;
    return result;
}

// ============================================================
// ICMP Ping -- 子网扫描
// ============================================================

int HostDiscovery::icmp_ping_subnet(uint32_t network_prefix,
                                     HostResult* out_results, int max_results) {
    uint32_t base = network_prefix & 0xFFFFFF00;
    int alive_count = 0;

    for (uint16_t host = 1; host <= 254 && alive_count < max_results; ++host) {
        uint32_t target_ip = base | htonl(host);
        HostResult result = icmp_ping(target_ip);

        if (result.scheduler.state == HostState::Up) {
            out_results[alive_count] = result;
            ++alive_count;
        }

        yield_cpu_();
    }

    return alive_count;
}

// ============================================================
// 综合主机发现：ARP + ICMP 双重检测
// ============================================================

int HostDiscovery::discover_subnet(uint32_t network_prefix,
                                    HostResult* out_results, int max_results) {
    uint32_t base = network_prefix & 0xFFFFFF00;
    int alive_count = 0;

    for (uint16_t host = 1; host <= 254 && alive_count < max_results; ++host) {
        uint32_t target_ip = base | htonl(host);

        HostResult arp_result = arp_scan(target_ip);
        if (arp_result.scheduler.state == HostState::Up) {
            out_results[alive_count] = arp_result;
            ++alive_count;
            continue;
        }

        HostResult icmp_result = icmp_ping(target_ip);
        if (icmp_result.scheduler.state == HostState::Up) {
            out_results[alive_count] = icmp_result;
            ++alive_count;
        }
    }

    return alive_count;
}

// ============================================================
// MAC 地址解析
// ============================================================

bool HostDiscovery::resolve_mac(uint32_t ip, uint8_t* out_mac) {
    if (!out_mac || !netif_) return false;

    const ip4_addr_t lwip_ip;
    ip4_addr_set_u32(const_cast<ip4_addr_t*>(&lwip_ip), ip);
    struct eth_addr* eth_ret = nullptr;

    err_t err = etharp_find_addr(netif_, &lwip_ip, &eth_ret, nullptr);
    if (err == ERR_OK && eth_ret) {
        for (int i = 0; i < MAC_ADDR_LEN; ++i) {
            out_mac[i] = eth_ret->addr[i];
        }
        return true;
    }

    HostResult result = arp_scan(ip);
    if (result.mac_resolved) {
        for (int i = 0; i < MAC_ADDR_LEN; ++i) {
            out_mac[i] = result.mac[i];
        }
        return true;
    }

    return false;
}

// ============================================================
// 构造并发送 ARP Request 以太网帧
// ============================================================

void HostDiscovery::send_arp_request_(uint32_t target_ip) {
    if (!netif_) return;

    ArpPacket pkt{};
    // 以太网头
    for (int i = 0; i < MAC_ADDR_LEN; ++i) pkt.eth_dst_mac[i] = 0xFF;
    for (int i = 0; i < MAC_ADDR_LEN; ++i) pkt.eth_src_mac[i] = src_mac_[i];
    pkt.eth_type = PP_HTONS(0x0806);

    // ARP 负载
    pkt.hw_type = PP_HTONS(1);
    pkt.proto_type = PP_HTONS(0x0800);
    pkt.hw_size = 6;
    pkt.proto_size = 4;
    pkt.opcode = PP_HTONS(1);
    for (int i = 0; i < MAC_ADDR_LEN; ++i) pkt.sender_mac[i] = src_mac_[i];
    pkt.sender_ip = src_ip_;
    for (int i = 0; i < MAC_ADDR_LEN; ++i) pkt.target_mac[i] = 0x00;
    pkt.target_ip = target_ip;

    struct pbuf* p = pbuf_alloc(PBUF_RAW, sizeof(ArpPacket), PBUF_RAM);
    if (p) {
        uint8_t* dst = static_cast<uint8_t*>(p->payload);
        const uint8_t* src = reinterpret_cast<const uint8_t*>(&pkt);
        for (size_t i = 0; i < sizeof(ArpPacket); ++i) {
            dst[i] = src[i];
        }
        netif_->linkoutput(netif_, p);
        pbuf_free(p);
    }
}

// ============================================================
// 构造并发送 ICMP Echo Request（通过 raw PCB）
// ============================================================

void HostDiscovery::send_icmp_echo_(struct raw_pcb* pcb, uint32_t target_ip,
                                      uint16_t seq) {
    constexpr int ICMP_HDR_SIZE = 8;
    constexpr int ICMP_DATA_SIZE = 32;
    constexpr int TOTAL_SIZE = ICMP_HDR_SIZE + ICMP_DATA_SIZE;

    struct pbuf* p = pbuf_alloc(PBUF_IP, TOTAL_SIZE, PBUF_RAM);
    if (!p) return;

    uint8_t* payload = static_cast<uint8_t*>(p->payload);
    payload[0] = 8; // Type: Echo Request
    payload[1] = 0; // Code
    payload[2] = 0; // Checksum (filled below)
    payload[3] = 0;
    payload[4] = static_cast<uint8_t>((icmp_seq_ >> 8) & 0xFF);
    payload[5] = static_cast<uint8_t>(icmp_seq_ & 0xFF);
    payload[6] = static_cast<uint8_t>((seq >> 8) & 0xFF);
    payload[7] = static_cast<uint8_t>(seq & 0xFF);

    for (int i = ICMP_HDR_SIZE; i < TOTAL_SIZE; ++i) {
        payload[i] = static_cast<uint8_t>(i - ICMP_HDR_SIZE);
    }

    uint16_t cksum = ip_chksum_pseudo(p, IP_PROTO_ICMP, p->tot_len,
                                       IP_ADDR_ANY,
                                       *reinterpret_cast<ip_addr_t*>(&target_ip));
    payload[2] = static_cast<uint8_t>((cksum >> 8) & 0xFF);
    payload[3] = static_cast<uint8_t>(cksum & 0xFF);

    ip_addr_t dst;
    ip_addr_set_ip4_u32(&dst, target_ip);
    raw_sendto(pcb, p, &dst);

    pbuf_free(p);
}

// ============================================================
// ICMP raw PCB 回调（静态分发到实例）
// ============================================================

uint8_t HostDiscovery::icmp_recv_callback_(void* arg, struct raw_pcb* /*pcb*/,
                                             struct pbuf* p,
                                             const ip_addr_t* addr) {
    HostDiscovery* self = static_cast<HostDiscovery*>(arg);
    if (!self || !p || !addr) {
        if (p) pbuf_free(p);
        return 0;
    }

    struct ip_hdr* iphdr = static_cast<struct ip_hdr*>(p->payload);
    if (IPH_PROTO(iphdr) == IP_PROTO_ICMP) {
        uint32_t reply_ip = ip4_addr_get_u32(ip_2_ip4(addr));
        if (reply_ip == self->pending_icmp_ip_) {
            self->icmp_reply_received_ = true;
            pbuf_free(p);
            return 1;
        }
    }

    pbuf_free(p);
    return 0;
}
