#ifndef AURORA_PROTOCOL_REGISTRY_HPP
#define AURORA_PROTOCOL_REGISTRY_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../kernel/core/mutex.hpp"

// ============================================================================
// ProtocolRegistry — L4 / 跨链路协议解复用与可插拔注册中心
//
// 设计原则（AGENTS.md §21/§27/§28/§36）：
//   1. 零动态内存分配：固定容量静态槽位表（MAX_HANDLERS = 16），全路径 noexcept。
//   2. 消除驱动与协议互相穿透：底层 LinkLayer 或 HCI 传输只负责将帧/段上送至
//      ProtocolRegistry，不知道上层挂接了 lwIP、DistributedSoftBus、BLE ATT/SMP
//      还是 PacketCapture。
//   3. 支持精确匹配 (ip_proto, port) 与通配符匹配 (PROTO_ANY = 0 / port = 0)，
//      按 (priority DESC, specificity DESC) 确定性顺序调用。
// ============================================================================

namespace auroraos {
namespace net {

// 数据帧流向
enum class PacketDirection : uint8_t {
    Rx = 0, // 入站（硬件驱动 -> 协议栈）
    Tx = 1, // 出站（协议栈 -> 硬件驱动）
    Ingress = Rx,
    Egress = Tx
};

// 协议处理器裁决结果
enum class ProtocolVerdict : uint8_t {
    Pass = 0,    // 已旁路观察或拷贝，继续向后续/低优先级协议栈（如 lwIP）传递
    Consume = 1, // 已由本协议独占消费，不再向下游协议传递
    Drop = 2     // 策略拒绝或校验失败，立即丢弃该报文
};

// 标准与扩展 L4 协议号常量
constexpr uint8_t PROTO_ANY = 0;         // 通配所有协议（供抓包/审计旁路挂载）
constexpr uint8_t PROTO_ICMP = 1;        // ICMPv4 (RFC 792)
constexpr uint8_t PROTO_TCP = 6;         // TCP (RFC 793)
constexpr uint8_t PROTO_UDP = 17;        // UDP (RFC 768)
constexpr uint8_t PROTO_BLE_L2CAP = 250; // BLE ACL L2CAP 通道（dst_port = CID，如 0x0004 ATT / 0x0006 SMP）
constexpr uint8_t PROTO_BLE_EVT = 251;   // BLE HCI Event 通道（dst_port = Event Code）

// L4 报文解复用上下文（主机字节序）
struct L4PacketContext {
    uint32_t src_ip{0};   // IPv4 源地址（主机字节序；非 IP 链路为 0）
    uint32_t dst_ip{0};   // IPv4 目的地址（主机字节序；非 IP 链路为 0）
    uint16_t src_port{0}; // 源端口（主机字节序；BLE 下为 connection_handle）
    uint16_t dst_port{0}; // 目的端口（主机字节序；BLE 下为 L2CAP CID 或 Event Code）
    uint8_t ip_proto{0};  // 协议号（PROTO_UDP / PROTO_TCP / PROTO_ICMP / PROTO_BLE_L2CAP 等）
    uint8_t tcp_flags{0}; // TCP 标志位（仅 ip_proto == PROTO_TCP 时有效）
    uint8_t src_mac[6]{}; // 源 MAC 地址（以太网源 MAC 或 BLE 对端 MAC）
    uint8_t dst_mac[6]{}; // 目的 MAC 地址（以太网目的 MAC）
    PacketDirection dir{PacketDirection::Rx};
    const char* ifname{"en0"};         // 关联链路接口名（如 "en0" / "ble0"）
    const uint8_t* raw_frame{nullptr}; // 指向完整 L2 帧的只读指针（纯 L4 注入时可为 nullptr）
    uint16_t raw_frame_len{0};         // 完整 L2 帧字节长度
};

// 面向对象 L4 协议处理器接口
class IProtocolHandler {
public:
    virtual ~IProtocolHandler() = default;

    // 协议处理器名称（用于诊断与去重识别）
    virtual const char* name() const noexcept = 0;

    // 处理解复用后的 L4 载荷
    virtual ProtocolVerdict on_packet(const L4PacketContext& ctx, const uint8_t* payload,
                                      size_t payload_len) noexcept = 0;
};

// 轻量函数指针协议回调签名
using ProtocolCallback = ProtocolVerdict (*)(const L4PacketContext& ctx, const uint8_t* payload, size_t payload_len,
                                             void* user_ctx);

class ProtocolRegistry {
public:
    static constexpr size_t MAX_HANDLERS = 16;
    static constexpr uint16_t PORT_ANY = 0;

    // 默认优先级约定（数值越大越优先执行）：
    //   PRIORITY_TAP      (200) : 被动抓包/安全审计（优先观察原始包，返回 Pass）
    //   PRIORITY_SECURITY (180) : 安全阻断策略（如高危端口拦截）
    //   PRIORITY_SERVICE  (120) : 专用端口协议（如 UDP 8899 DistributedSoftBus、BLE ATT/SMP）
    //   PRIORITY_DEFAULT  (100) : 常规注册协议
    //   PRIORITY_FALLBACK (50)  : 通用协议栈兜底（如 lwIP TCP/UDP/ICMP 全端口通配）
    static constexpr uint8_t PRIORITY_TAP = 200;
    static constexpr uint8_t PRIORITY_SECURITY = 180;
    static constexpr uint8_t PRIORITY_SERVICE = 120;
    static constexpr uint8_t PRIORITY_DEFAULT = 100;
    static constexpr uint8_t PRIORITY_FALLBACK = 50;

    static ProtocolRegistry& instance() noexcept {
        static ProtocolRegistry reg;
        return reg;
    }

    ProtocolRegistry() noexcept = default;

    // 注册面向对象协议处理器（若同一 (ip_proto, port, handler) 已存在则原地更新优先级，幂等不重复占用槽位）
    bool register_handler(uint8_t ip_proto, uint16_t port, IProtocolHandler* handler,
                          uint8_t priority = PRIORITY_DEFAULT) noexcept {
        if (!handler) {
            return false;
        }
        LockGuard lock(mutex_);
        for (size_t i = 0; i < MAX_HANDLERS; ++i) {
            if (entries_[i].used && entries_[i].handler == handler && entries_[i].ip_proto == ip_proto &&
                entries_[i].port == port) {
                entries_[i].priority = priority;
                return true;
            }
        }
        for (size_t i = 0; i < MAX_HANDLERS; ++i) {
            if (!entries_[i].used) {
                entries_[i].used = true;
                entries_[i].ip_proto = ip_proto;
                entries_[i].port = port;
                entries_[i].priority = priority;
                entries_[i].handler = handler;
                entries_[i].callback = nullptr;
                entries_[i].user_ctx = nullptr;
                copy_name(entries_[i].cb_name, handler->name());
                return true;
            }
        }
        return false;
    }

    // 注册函数指针形式协议回调
    bool register_callback(const char* name, uint8_t ip_proto, uint16_t port, ProtocolCallback cb,
                           void* user_ctx = nullptr, uint8_t priority = PRIORITY_DEFAULT) noexcept {
        if (!cb) {
            return false;
        }
        LockGuard lock(mutex_);
        for (size_t i = 0; i < MAX_HANDLERS; ++i) {
            if (entries_[i].used && entries_[i].callback == cb && entries_[i].user_ctx == user_ctx &&
                entries_[i].ip_proto == ip_proto && entries_[i].port == port) {
                entries_[i].priority = priority;
                copy_name(entries_[i].cb_name, name);
                return true;
            }
        }
        for (size_t i = 0; i < MAX_HANDLERS; ++i) {
            if (!entries_[i].used) {
                entries_[i].used = true;
                entries_[i].ip_proto = ip_proto;
                entries_[i].port = port;
                entries_[i].priority = priority;
                entries_[i].handler = nullptr;
                entries_[i].callback = cb;
                entries_[i].user_ctx = user_ctx;
                copy_name(entries_[i].cb_name, name);
                return true;
            }
        }
        return false;
    }

    // 按处理器指针注销所有关联槽位
    bool unregister_handler(const IProtocolHandler* handler) noexcept {
        if (!handler) {
            return false;
        }
        LockGuard lock(mutex_);
        bool removed = false;
        for (size_t i = 0; i < MAX_HANDLERS; ++i) {
            if (entries_[i].used && entries_[i].handler == handler) {
                entries_[i] = Entry{};
                removed = true;
            }
        }
        return removed;
    }

    // 按 (ip_proto, port) 注销槽位
    bool unregister_port(uint8_t ip_proto, uint16_t port) noexcept {
        LockGuard lock(mutex_);
        bool removed = false;
        for (size_t i = 0; i < MAX_HANDLERS; ++i) {
            if (entries_[i].used && entries_[i].ip_proto == ip_proto && entries_[i].port == port) {
                entries_[i] = Entry{};
                removed = true;
            }
        }
        return removed;
    }

    bool has_handler(const IProtocolHandler* handler) const noexcept {
        if (!handler) {
            return false;
        }
        for (size_t i = 0; i < MAX_HANDLERS; ++i) {
            if (entries_[i].used && entries_[i].handler == handler) {
                return true;
            }
        }
        return false;
    }

    // 查找匹配 (ip_proto, port) 的最高优先级处理器（优先精确匹配，其次通配符）
    IProtocolHandler* find_handler(uint8_t ip_proto, uint16_t port) const noexcept {
        int best_idx = -1;
        uint16_t best_score = 0;
        for (size_t i = 0; i < MAX_HANDLERS; ++i) {
            if (!entries_[i].used || !entries_[i].handler) {
                continue;
            }
            if (!matches(entries_[i], ip_proto, port)) {
                continue;
            }
            const uint16_t score = compute_score(entries_[i]);
            if (best_idx < 0 || score > best_score) {
                best_idx = static_cast<int>(i);
                best_score = score;
            }
        }
        return (best_idx >= 0) ? entries_[best_idx].handler : nullptr;
    }

    size_t handler_count() const noexcept {
        size_t count = 0;
        for (size_t i = 0; i < MAX_HANDLERS; ++i) {
            if (entries_[i].used) {
                ++count;
            }
        }
        return count;
    }

    // 直接分发已解析的 L4 报文上下文与载荷
    ProtocolVerdict dispatch_l4(const L4PacketContext& ctx, const uint8_t* payload, size_t payload_len) noexcept {
        dispatched_count_++;

        // 收集匹配槽位的「回调三元组 + 分值」快照后释放锁再调用：
        // 1) 回调可能再入本类（收发联动），锁外调用避免自死锁；
        // 2) 并发注销/注册期间快照保证不撕裂槽位、不调用半初始化条目。
        struct Match {
            IProtocolHandler* handler;
            ProtocolCallback callback;
            void* user_ctx;
            uint16_t score;
        };
        Match matched[MAX_HANDLERS];
        size_t match_count = 0;

        {
            LockGuard lock(mutex_);
            for (size_t i = 0; i < MAX_HANDLERS; ++i) {
                if (!entries_[i].used) {
                    continue;
                }
                // 对于 TCP/UDP，入站按 dst_port 匹配，出站若 dst_port 不匹配也可匹配 src_port（如源端口固定服务）
                const bool port_match = matches(entries_[i], ctx.ip_proto, ctx.dst_port) ||
                                        (ctx.dir == PacketDirection::Tx && ctx.src_port != 0 &&
                                         matches(entries_[i], ctx.ip_proto, ctx.src_port));
                if (!port_match) {
                    continue;
                }
                matched[match_count].handler = entries_[i].handler;
                matched[match_count].callback = entries_[i].callback;
                matched[match_count].user_ctx = entries_[i].user_ctx;
                matched[match_count].score = compute_score(entries_[i]);
                ++match_count;
            }
        }

        // 小规模插入排序（最多 16 项，零堆分配）
        for (size_t i = 1; i < match_count; ++i) {
            const Match val = matched[i];
            size_t j = i;
            while (j > 0 && matched[j - 1].score < val.score) {
                matched[j] = matched[j - 1];
                --j;
            }
            matched[j] = val;
        }

        for (size_t k = 0; k < match_count; ++k) {
            ProtocolVerdict v = ProtocolVerdict::Pass;
            if (matched[k].handler) {
                v = matched[k].handler->on_packet(ctx, payload, payload_len);
            } else if (matched[k].callback) {
                v = matched[k].callback(ctx, payload, payload_len, matched[k].user_ctx);
            }
            if (v == ProtocolVerdict::Drop) {
                dropped_count_++;
                return ProtocolVerdict::Drop;
            }
            if (v == ProtocolVerdict::Consume) {
                consumed_count_++;
                return ProtocolVerdict::Consume;
            }
        }
        return ProtocolVerdict::Pass;
    }

    // 从标准以太网 II 帧（含 IPv4 + TCP/UDP/ICMP 头）解析并分发到 L4 协议注册表。
    // 对非 IPv4 帧（如 ARP 0x0806），仅触发注册在 PROTO_ANY (0, 0) 的通配观察器。
    ProtocolVerdict dispatch_ethernet_frame(const uint8_t* frame, int len, PacketDirection dir = PacketDirection::Rx,
                                            const char* ifname = "en0") noexcept {
        if (!frame || len < 14) {
            return ProtocolVerdict::Pass;
        }

        const uint16_t eth_type = static_cast<uint16_t>((static_cast<uint16_t>(frame[12]) << 8) | frame[13]);
        if (eth_type != 0x0800) {
            // 非 IPv4 帧：仅向 PROTO_ANY 通配处理器投递原始帧上下文
            L4PacketContext ctx{};
            ctx.ip_proto = PROTO_ANY;
            memcpy(ctx.dst_mac, frame, 6);
            memcpy(ctx.src_mac, frame + 6, 6);
            ctx.dir = dir;
            ctx.ifname = ifname ? ifname : "en0";
            ctx.raw_frame = frame;
            ctx.raw_frame_len = static_cast<uint16_t>(len);
            return dispatch_l4(ctx, frame + 14, static_cast<size_t>(len - 14));
        }

        // IPv4 最小头长度校验（14B Ethernet + 20B IPv4 = 34B）
        if (len < 34) {
            return ProtocolVerdict::Pass;
        }
        const uint8_t version = (frame[14] >> 4) & 0x0F;
        const uint8_t ihl = frame[14] & 0x0F;
        if (version != 4 || ihl < 5) {
            return ProtocolVerdict::Pass;
        }
        const int ip_hdr_len = static_cast<int>(ihl) * 4;
        const int l4_offset = 14 + ip_hdr_len;
        if (len < l4_offset) {
            return ProtocolVerdict::Pass;
        }

        // 解析 IPv4 总长度字段（若帧尾带以太网 60B padding，则按 IP total_len 裁剪载荷）
        const uint16_t ip_total_len = read_be16(frame + 16);
        int effective_frame_len = len;
        if (ip_total_len >= static_cast<uint16_t>(ip_hdr_len) && 14 + static_cast<int>(ip_total_len) <= len) {
            effective_frame_len = 14 + static_cast<int>(ip_total_len);
        }

        L4PacketContext ctx{};
        memcpy(ctx.dst_mac, frame, 6);
        memcpy(ctx.src_mac, frame + 6, 6);
        ctx.ip_proto = frame[23];
        ctx.src_ip = read_be32(frame + 26);
        ctx.dst_ip = read_be32(frame + 30);
        ctx.dir = dir;
        ctx.ifname = ifname ? ifname : "en0";
        ctx.raw_frame = frame;
        ctx.raw_frame_len = static_cast<uint16_t>(len);

        const uint8_t* l4_hdr = frame + l4_offset;
        const int l4_total_len = effective_frame_len - l4_offset;
        const uint8_t* l4_payload = l4_hdr;
        size_t l4_payload_len = static_cast<size_t>(l4_total_len);

        if (ctx.ip_proto == PROTO_UDP) {
            if (l4_total_len < 8) {
                return ProtocolVerdict::Pass;
            }
            ctx.src_port = read_be16(l4_hdr);
            ctx.dst_port = read_be16(l4_hdr + 2);
            const uint16_t udp_len = read_be16(l4_hdr + 4);
            l4_payload = l4_hdr + 8;
            if (udp_len >= 8 && static_cast<int>(udp_len) <= l4_total_len) {
                l4_payload_len = static_cast<size_t>(udp_len - 8);
            } else {
                l4_payload_len = static_cast<size_t>(l4_total_len - 8);
            }
        } else if (ctx.ip_proto == PROTO_TCP) {
            if (l4_total_len < 20) {
                return ProtocolVerdict::Pass;
            }
            ctx.src_port = read_be16(l4_hdr);
            ctx.dst_port = read_be16(l4_hdr + 2);
            const uint8_t data_off = (l4_hdr[12] >> 4) & 0x0F;
            const int tcp_hdr_len = static_cast<int>(data_off) * 4;
            if (data_off < 5 || l4_total_len < tcp_hdr_len) {
                return ProtocolVerdict::Pass;
            }
            ctx.tcp_flags = l4_hdr[13];
            l4_payload = l4_hdr + tcp_hdr_len;
            l4_payload_len = static_cast<size_t>(l4_total_len - tcp_hdr_len);
        } else if (ctx.ip_proto == PROTO_ICMP) {
            if (l4_total_len < 4) {
                return ProtocolVerdict::Pass;
            }
            // ICMP: src_port 存放 (type << 8 | code) 便于诊断
            ctx.src_port = static_cast<uint16_t>((static_cast<uint16_t>(l4_hdr[0]) << 8) | l4_hdr[1]);
            ctx.dst_port = 0;
            l4_payload = l4_hdr + 4;
            l4_payload_len = static_cast<size_t>(l4_total_len - 4);
        }

        return dispatch_l4(ctx, l4_payload, l4_payload_len);
    }

    uint32_t dispatched_count() const noexcept {
        return dispatched_count_;
    }

    uint32_t consumed_count() const noexcept {
        return consumed_count_;
    }

    uint32_t dropped_count() const noexcept {
        return dropped_count_;
    }

    void reset_stats() noexcept {
        dispatched_count_ = 0;
        consumed_count_ = 0;
        dropped_count_ = 0;
    }

    void reset() noexcept {
        for (size_t i = 0; i < MAX_HANDLERS; ++i) {
            entries_[i] = Entry{};
        }
        reset_stats();
    }

private:
    struct Entry {
        bool used{false};
        uint8_t ip_proto{0};
        uint16_t port{0};
        uint8_t priority{PRIORITY_DEFAULT};
        IProtocolHandler* handler{nullptr};
        ProtocolCallback callback{nullptr};
        void* user_ctx{nullptr};
        char cb_name[24]{};
    };

    Entry entries_[MAX_HANDLERS]{};
    // 注册/注销/分发共用互斥（与 DeviceRouteTable/PacketCapture 对齐）。
    // 回调一律在锁外调用（见 dispatch_l4 快照），处理器对象须为长命静态对象。
    mutable Mutex mutex_;
    uint32_t dispatched_count_{0};
    uint32_t consumed_count_{0};
    uint32_t dropped_count_{0};

    static bool matches(const Entry& e, uint8_t ip_proto, uint16_t port) noexcept {
        const bool proto_ok = (e.ip_proto == PROTO_ANY) || (e.ip_proto == ip_proto);
        const bool port_ok = (e.port == 0) || (e.port == port);
        return proto_ok && port_ok;
    }

    // 排序分值：高 8 位为 priority，低 8 位为精确度（proto+port 精确匹配=3，仅 proto=2，仅 port=1，全通配=0）
    static uint16_t compute_score(const Entry& e) noexcept {
        uint8_t specificity = 0;
        if (e.ip_proto != PROTO_ANY) {
            specificity = static_cast<uint8_t>(specificity + 2u);
        }
        if (e.port != 0) {
            specificity = static_cast<uint8_t>(specificity + 1u);
        }
        return static_cast<uint16_t>((static_cast<uint16_t>(e.priority) << 8) | specificity);
    }

    static void copy_name(char dst[24], const char* src) noexcept {
        if (!src) {
            dst[0] = '\0';
            return;
        }
        size_t i = 0;
        while (i < 23 && src[i] != '\0') {
            dst[i] = src[i];
            ++i;
        }
        dst[i] = '\0';
    }

    static uint16_t read_be16(const uint8_t* p) noexcept {
        return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
    }

    static uint32_t read_be32(const uint8_t* p) noexcept {
        return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
    }
};

} // namespace net
} // namespace auroraos

#endif // AURORA_PROTOCOL_REGISTRY_HPP
