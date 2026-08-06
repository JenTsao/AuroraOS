#ifndef AURORA_SCANNER_HOST_DISCOVERY_HPP
#define AURORA_SCANNER_HOST_DISCOVERY_HPP

#include <stdint.h>
#include <stddef.h>
#include "../../kernel/mutex.hpp"

extern "C" {
#include "lwip/sockets.h"
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
//   - 子网扫描: 遍历 IP 范围，合并 ARP + ICMP 结果确定主机存活状态
//   - 遵循 C++ Core Guidelines
// ============================================================

#ifndef MAC_ADDR_LEN
constexpr int MAC_ADDR_LEN = 6;
#endif

enum class HostState : uint8_t {
    Up      = 0,  // 主机在线
    Down    = 1,  // 主机离线
    Unknown = 2   // 无法判定
};

struct HostResult {
    uint32_t  ip;                    // 目标 IP（网络字节序）
    uint8_t   mac[MAC_ADDR_LEN];     // MAC 地址（ARP 发现时填充）
    HostState state;                 // 存活状态
    uint32_t  latency_ms;            // 延迟（毫秒）
    bool      mac_resolved;          // 是否成功解析 MAC
};

// ARP 以太网帧布局（手动构造，避免依赖 lwIP 内部结构）
struct __attribute__((packed)) ArpPacket {
    // 以太网头（14 字节）
    uint8_t  eth_dst_mac[MAC_ADDR_LEN];  // FF:FF:FF:FF:FF:FF（广播）
    uint8_t  eth_src_mac[MAC_ADDR_LEN];  // 本机 MAC
    uint16_t eth_type;                   // 0x0806（ARP）

    // ARP 负载（28 字节）
    uint16_t hw_type;       // 1 = Ethernet
    uint16_t proto_type;    // 0x0800 = IPv4
    uint8_t  hw_size;       // 6
    uint8_t  proto_size;    // 4
    uint16_t opcode;        // 1 = REQUEST, 2 = REPLY
    uint8_t  sender_mac[MAC_ADDR_LEN];
    uint32_t sender_ip;
    uint8_t  target_mac[MAC_ADDR_LEN];  // 00:00:00:00:00:00（未知）
    uint32_t target_ip;
};

class HostDiscovery {
public:
    // 初始化：绑定本机网络接口和 MAC
    void init(struct netif* netif) {
        netif_ = netif;
        if (netif_) {
            for (int i = 0; i < MAC_ADDR_LEN; ++i) {
                src_mac_[i] = netif_->hwaddr[i];
            }
            src_ip_ = ip4_addr_get_u32(&netif_->ip_addr);
        }
    }

    // 设置超时（毫秒），默认 1500ms
    void set_timeout(uint32_t timeout_ms) {
        timeout_ms_ = timeout_ms;
    }

    // 设置重试次数，默认 2
    void set_retries(uint8_t retries) {
        retries_ = retries;
    }

    // ---- ARP 扫描 ----

    // 对单个 IP 执行 ARP 请求，阻塞等待 Reply
    // 注意：此方法非线程安全——同一实例不可并发调用
    HostResult arp_scan(uint32_t target_ip) {
        LockGuard lock(scan_mutex_);

        HostResult result{};
        result.ip = target_ip;
        result.state = HostState::Down;
        result.mac_resolved = false;

        if (!netif_) {
            result.state = HostState::Unknown;
            return result;
        }

        // 保存回调上下文
        pending_ip_ = target_ip;
        arp_reply_received_ = false;

        // 发送 ARP Request
        for (uint8_t r = 0; r < retries_; ++r) {
            send_arp_request_(target_ip);

            // 轮询等待 ARP Reply（非阻塞，每次轮询让出CPU）
            uint32_t start = get_tick_count_();
            while ((get_tick_count_() - start) < timeout_ms_) {
                yield_cpu_();

                // 检查 lwIP ARP 表是否有新条目
                err_t arp_err = etharp_query(netif_, reinterpret_cast<const ip4_addr_t*>(&target_ip), nullptr);
                if (arp_err == ERR_OK) {
                    // etharp_query 查询现有条目 — 返回 ERR_OK 表示 ARP 表内已有 MAC
                    // 尝试获取实际 MAC
                    struct eth_addr* eth_ret = nullptr;
                    const ip4_addr_t ip;
                    ip4_addr_set_u32(const_cast<ip4_addr_t*>(&ip), target_ip);
                    err_t find_err = etharp_find_addr(netif_, &ip, &eth_ret, nullptr);
                    if (find_err == ERR_OK && eth_ret) {
                        for (int i = 0; i < MAC_ADDR_LEN; ++i) {
                            result.mac[i] = eth_ret->addr[i];
                        }
                        result.state = HostState::Up;
                        result.mac_resolved = true;
                        result.latency_ms = get_tick_count_() - start;
                        return result;
                    }
                }

                // 如果已收到 ARP Reply 的回调信号
                if (arp_reply_received_) {
                    result.state = HostState::Up;
                    result.mac_resolved = true;
                    result.latency_ms = get_tick_count_() - start;
                    // 从 ARP 表读取 MAC
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

        // 超时：主机离线
        result.state = HostState::Down;
        result.latency_ms = timeout_ms_ * retries_;
        return result;
    }

    // ARP 扫描整个子网（/24）
    //   network_prefix: 子网前缀（如 192.168.1.1→取前3字节，即 192.168.1.0/24）
    //   out_results: 结果输出缓冲区
    //   max_results: 缓冲区容量
    //   返回: 发现的存活主机数
    int arp_scan_subnet(uint32_t network_prefix, HostResult* out_results, int max_results) {
        // 取 /24 前缀（保留前 3 字节）
        uint32_t base = network_prefix & 0xFFFFFF00;
        int alive_count = 0;

        // 扫描 .1 到 .254（排除 .0 网络地址和 .255 广播地址）
        for (uint16_t host = 1; host <= 254 && alive_count < max_results; ++host) {
            uint32_t target_ip = base | htonl(host);
            HostResult result = arp_scan(target_ip);

            if (result.state == HostState::Up) {
                out_results[alive_count] = result;
                ++alive_count;
            }

            yield_cpu_(); // 防止饿死协议栈
        }

        return alive_count;
    }

    // ---- ICMP Ping ----

    // ICMP Echo 请求（单次 Ping）
    //   使用 lwIP raw PCB 发送 ICMP Echo Request
    //   target_ip: 目标 IP（网络字节序）
    //   返回: HostResult
    // 注意：此方法非线程安全——同一实例不可并发调用
    HostResult icmp_ping(uint32_t target_ip) {
        LockGuard lock(scan_mutex_);

        HostResult result{};
        result.ip = target_ip;
        result.state = HostState::Down;
        result.mac_resolved = false;

        // 注册 ICMP 回调接收 Echo Reply
        pending_icmp_ip_ = target_ip;
        icmp_reply_received_ = false;

        struct raw_pcb* pcb = raw_new(IP_PROTO_ICMP);
        if (!pcb) {
            result.state = HostState::Unknown;
            return result;
        }

        raw_recv(pcb, icmp_recv_callback_);
        raw_bind(pcb, IP_ADDR_ANY);
        pcb->recv_arg = this;  // 传递实例指针给回调

        // 发送 ICMP Echo Request
        for (uint8_t r = 0; r < retries_; ++r) {
            send_icmp_echo_(pcb, target_ip, static_cast<uint16_t>(icmp_seq_));
            ++icmp_seq_;

            uint32_t start = get_tick_count_();
            while ((get_tick_count_() - start) < timeout_ms_) {
                yield_cpu_();

                if (icmp_reply_received_ && pending_icmp_ip_ == target_ip) {
                    result.state = HostState::Up;
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

    // ICMP Ping 扫描子网
    int icmp_ping_subnet(uint32_t network_prefix, HostResult* out_results, int max_results) {
        uint32_t base = network_prefix & 0xFFFFFF00;
        int alive_count = 0;

        for (uint16_t host = 1; host <= 254 && alive_count < max_results; ++host) {
            uint32_t target_ip = base | htonl(host);
            HostResult result = icmp_ping(target_ip);

            if (result.state == HostState::Up) {
                out_results[alive_count] = result;
                ++alive_count;
            }

            yield_cpu_();
        }

        return alive_count;
    }

    // ---- 综合主机发现 ----
    // ARP + ICMP 双重检测，任一方法报告在线即判定为 Up
    int discover_subnet(uint32_t network_prefix, HostResult* out_results, int max_results) {
        uint32_t base = network_prefix & 0xFFFFFF00;
        int alive_count = 0;

        for (uint16_t host = 1; host <= 254 && alive_count < max_results; ++host) {
            uint32_t target_ip = base | htonl(host);

            // 先 ARP（同子网内最可靠）
            HostResult arp_result = arp_scan(target_ip);

            if (arp_result.state == HostState::Up) {
                out_results[alive_count] = arp_result;
                ++alive_count;
                continue;
            }

            // ARP 失败则尝试 ICMP（可能跨子网或被防火墙屏蔽ARP）
            HostResult icmp_result = icmp_ping(target_ip);
            if (icmp_result.state == HostState::Up) {
                out_results[alive_count] = icmp_result;
                ++alive_count;
            }
        }

        return alive_count;
    }

    // ---- MAC 解析 ----
    // 通过 ARP 解析单个 IP 的 MAC 地址
    bool resolve_mac(uint32_t ip, uint8_t* out_mac) {
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

        // ARP 表中没有，发送 ARP 请求并等待
        HostResult result = arp_scan(ip);
        if (result.mac_resolved) {
            for (int i = 0; i < MAC_ADDR_LEN; ++i) {
                out_mac[i] = result.mac[i];
            }
            return true;
        }

        return false;
    }

    // ---- 工具方法 ----

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
    Mutex    scan_mutex_;  // 序列化并发扫描操作

    // ARP 回调状态（每实例）
    volatile bool arp_reply_received_ = false;
    uint32_t      pending_ip_ = 0;

    // ICMP 回调状态（每实例）
    volatile bool icmp_reply_received_ = false;
    uint32_t      pending_icmp_ip_ = 0;
    uint16_t      icmp_seq_ = 0;

    // ICMP raw PCB 回调（静态分发到实例）
    static uint8_t icmp_recv_callback_(void* arg, struct raw_pcb* pcb, struct pbuf* p,
                                        const ip_addr_t* addr) {
        HostDiscovery* self = static_cast<HostDiscovery*>(arg);
        if (!self || !p || !addr) {
            if (p) pbuf_free(p);
            return 0;
        }

        struct ip_hdr* iphdr = static_cast<struct ip_hdr*>(p->payload);
        // 确认是 ICMP
        if (IPH_PROTO(iphdr) == IP_PROTO_ICMP) {
            uint32_t reply_ip = ip4_addr_get_u32(ip_2_ip4(addr));
            if (reply_ip == self->pending_icmp_ip_) {
                self->icmp_reply_received_ = true;
                pbuf_free(p);
                return 1; // 已消费
            }
        }

        pbuf_free(p);
        return 0; // 未消费
    }

    // 构造并发送 ARP Request 以太网帧
    void send_arp_request_(uint32_t target_ip) {
        if (!netif_) return;

        ArpPacket pkt{};
        // 以太网头
        for (int i = 0; i < MAC_ADDR_LEN; ++i) pkt.eth_dst_mac[i] = 0xFF; // 广播
        for (int i = 0; i < MAC_ADDR_LEN; ++i) pkt.eth_src_mac[i] = src_mac_[i];
        pkt.eth_type = PP_HTONS(0x0806); // ARP

        // ARP 负载
        pkt.hw_type = PP_HTONS(1);     // Ethernet
        pkt.proto_type = PP_HTONS(0x0800); // IPv4
        pkt.hw_size = 6;
        pkt.proto_size = 4;
        pkt.opcode = PP_HTONS(1);      // REQUEST
        for (int i = 0; i < MAC_ADDR_LEN; ++i) pkt.sender_mac[i] = src_mac_[i];
        pkt.sender_ip = src_ip_;
        for (int i = 0; i < MAC_ADDR_LEN; ++i) pkt.target_mac[i] = 0x00;
        pkt.target_ip = target_ip;

        // 通过 netif 发送原始以太网帧
        struct pbuf* p = pbuf_alloc(PBUF_RAW, sizeof(ArpPacket), PBUF_RAM);
        if (p) {
            // 将 ARP 包拷贝到 pbuf 负载区
            uint8_t* dst = static_cast<uint8_t*>(p->payload);
            const uint8_t* src = reinterpret_cast<const uint8_t*>(&pkt);
            for (size_t i = 0; i < sizeof(ArpPacket); ++i) {
                dst[i] = src[i];
            }
            netif_->linkoutput(netif_, p);
            pbuf_free(p);
        }
    }

    // 构造并发送 ICMP Echo Request（通过 raw PCB）
    void send_icmp_echo_(struct raw_pcb* pcb, uint32_t target_ip, uint16_t seq) {
        // 构造 ICMP Echo Request
        constexpr int ICMP_HDR_SIZE = 8;
        constexpr int ICMP_DATA_SIZE = 32;
        constexpr int TOTAL_SIZE = ICMP_HDR_SIZE + ICMP_DATA_SIZE;

        struct pbuf* p = pbuf_alloc(PBUF_IP, TOTAL_SIZE, PBUF_RAM);
        if (!p) return;

        uint8_t* payload = static_cast<uint8_t*>(p->payload);
        // ICMP Type = 8 (Echo Request), Code = 0
        payload[0] = 8; // Type: Echo Request
        payload[1] = 0; // Code
        payload[2] = 0; // Checksum (0 for now)
        payload[3] = 0;
        payload[4] = static_cast<uint8_t>((icmp_seq_ >> 8) & 0xFF); // ID
        payload[5] = static_cast<uint8_t>(icmp_seq_ & 0xFF);
        payload[6] = static_cast<uint8_t>((seq >> 8) & 0xFF); // Sequence
        payload[7] = static_cast<uint8_t>(seq & 0xFF);

        // 填充数据（简单递增模式）
        for (int i = ICMP_HDR_SIZE; i < TOTAL_SIZE; ++i) {
            payload[i] = static_cast<uint8_t>(i - ICMP_HDR_SIZE);
        }

        // 计算 ICMP 校验和
        uint16_t cksum = ip_chksum_pseudo(p, IP_PROTO_ICMP, p->tot_len,
                                           IP_ADDR_ANY, *reinterpret_cast<ip_addr_t*>(&target_ip));
        payload[2] = static_cast<uint8_t>((cksum >> 8) & 0xFF);
        payload[3] = static_cast<uint8_t>(cksum & 0xFF);

        ip_addr_t dst;
        ip_addr_set_ip4_u32(&dst, target_ip);
        raw_sendto(pcb, p, &dst);

        pbuf_free(p);
    }

    static uint32_t get_tick_count_() {
        extern volatile uint32_t tick_count;
        return tick_count;
    }

    static void yield_cpu_() {
        extern void sys_yield();
        sys_yield();
    }
};

#endif // AURORA_SCANNER_HOST_DISCOVERY_HPP
