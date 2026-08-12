#ifndef AURORA_SCANNER_HOST_DISCOVERY_HPP
#define AURORA_SCANNER_HOST_DISCOVERY_HPP

#include <stdint.h>
#include <stddef.h>
#include "../../kernel/core/mutex.hpp"

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

// ============================================================
// Host Discovery -- ARP 扫描 / ICMP 探测引擎
//
// 设计：
//   - ARP Scan: 构造原始以太网 ARP Request 帧，通过 netif->linkoutput 发送
//   - ICMP Ping: 利用 lwIP ICMP raw PCB 发送 Echo Request，回调收 Echo Reply
//   - 子网扫描: 遍历 IP 范围，合并 ARP + ICMP 结果
//   - .cpp 实现避免嵌入式内联代码膨胀
// ============================================================

#ifndef MAC_ADDR_LEN
constexpr int MAC_ADDR_LEN = 6;
#endif

enum class HostState : uint8_t {
    Up      = 0,
    Down    = 1,
    Unknown = 2
};

struct HostResult {
    uint32_t  ip;
    uint8_t   mac[MAC_ADDR_LEN];
    HostState state;
    uint32_t  latency_ms;
    bool      mac_resolved;
};

struct __attribute__((packed)) ArpPacket {
    uint8_t  eth_dst_mac[MAC_ADDR_LEN];
    uint8_t  eth_src_mac[MAC_ADDR_LEN];
    uint16_t eth_type;
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t  hw_size;
    uint8_t  proto_size;
    uint16_t opcode;
    uint8_t  sender_mac[MAC_ADDR_LEN];
    uint32_t sender_ip;
    uint8_t  target_mac[MAC_ADDR_LEN];
    uint32_t target_ip;
};

class HostDiscovery {
public:
    // ---- 初始化与配置 (内联) ----

    void init(struct netif* netif) {
        netif_ = netif;
        if (netif_) {
            for (int i = 0; i < MAC_ADDR_LEN; ++i) {
                src_mac_[i] = netif_->hwaddr[i];
            }
            src_ip_ = ip4_addr_get_u32(&netif_->ip_addr);
        }
    }

    void set_timeout(uint32_t timeout_ms) { timeout_ms_ = timeout_ms; }
    void set_retries(uint8_t retries)     { retries_ = retries; }

    // ---- ARP 扫描 (.cpp 实现) ----

    HostResult arp_scan(uint32_t target_ip);
    int arp_scan_subnet(uint32_t network_prefix, HostResult* out_results,
                        int max_results);

    // ---- ICMP Ping (.cpp 实现) ----

    HostResult icmp_ping(uint32_t target_ip);
    int icmp_ping_subnet(uint32_t network_prefix, HostResult* out_results,
                         int max_results);

    // ---- 综合主机发现 (.cpp 实现) ----

    int discover_subnet(uint32_t network_prefix, HostResult* out_results,
                        int max_results);

    // ---- MAC 解析 (.cpp 实现) ----

    bool resolve_mac(uint32_t ip, uint8_t* out_mac);

    // ---- 工具方法 (内联) ----

    static const char* host_state_to_string(HostState state) {
        switch (state) {
            case HostState::Up:      return "up";
            case HostState::Down:    return "down";
            case HostState::Unknown: return "unknown";
            default:                 return "invalid";
        }
    }

private:
    struct netif* netif_ = nullptr;
    uint8_t  src_mac_[MAC_ADDR_LEN]{};
    uint32_t src_ip_ = 0;
    uint32_t timeout_ms_ = 1500;
    uint8_t  retries_ = 2;
    Mutex    scan_mutex_;

    volatile bool arp_reply_received_ = false;
    uint32_t      pending_ip_ = 0;

    volatile bool icmp_reply_received_ = false;
    uint32_t      pending_icmp_ip_ = 0;
    uint16_t      icmp_seq_ = 0;

    static uint8_t icmp_recv_callback_(void* arg, struct raw_pcb* pcb,
                                        struct pbuf* p, const ip_addr_t* addr);
    void send_arp_request_(uint32_t target_ip);
    void send_icmp_echo_(struct raw_pcb* pcb, uint32_t target_ip, uint16_t seq);
    static uint32_t get_tick_count_();
    static void yield_cpu_();
};

#endif // AURORA_SCANNER_HOST_DISCOVERY_HPP
