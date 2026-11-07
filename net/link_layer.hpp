#ifndef AURORA_LINK_LAYER_HPP
#define AURORA_LINK_LAYER_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "net_device.hpp"
#include "protocol_registry.hpp"
#include "../kernel/core/mutex.hpp"

// ============================================================================
// LinkLayer — 可插拔 L2 链路层抽象与收发管线中枢
//
// 职责与架构边界（AGENTS.md §5/§8/§27/§28）：
//   1. 统一管理底层物理/虚拟链路设备（NetDevice：StellarisEth、虚拟 en0、BLE 链路等），
//      向上屏蔽具体硬件寄存器与驱动类型。
//   2. 提供可插拔旁路嗅探槽位（ILinkTap：如 PacketCapture / NIDS）与策略过滤槽位
//      （ILinkFilter：如 Firewall），使 adapter/net/ethernetif.cpp 与 PacketCapture
//      不再硬编码依赖具体驱动或彼此内部实现。
//   3. 将入站/出站帧自动桥接至 L4 ProtocolRegistry 与注册的 ILinkProtocolHandler
//      （lwIP、BLE HCI Event/ACL、自定义 L2 协议）。
//   4. 全路径零堆分配（固定容量静态表），noexcept。
// ============================================================================

namespace auroraos {
namespace net {

// 物理/逻辑链路媒介类型
enum class LinkType : uint8_t {
    Ethernet = 1,  // IEEE 802.3 以太网帧（如 StellarisEth "en0"）
    BleHci = 2,    // Bluetooth LE HCI H4 帧（如 HciUartTransport "ble0"）
    IEEE80211 = 3, // WiFi 802.11 帧（如 RTL8187L/RTL8812AU "wlan0"）
    SerialRpc = 4, // 串行 SoftBus RPC 帧（"rpc0"）
    Loopback = 5   // 本地环回（"lo0"）
};

// 被动链路旁路观察器（如 PacketCapture、IdsEngine）
class ILinkTap {
public:
    virtual ~ILinkTap() = default;

    virtual const char* tap_name() const noexcept {
        return "link_tap";
    }

    virtual void on_frame(const uint8_t* frame, int len, PacketDirection dir, LinkType link_type,
                          const char* ifname) noexcept = 0;
};

using LinkTapFn = void (*)(const uint8_t* frame, int len, PacketDirection dir, LinkType link_type, const char* ifname,
                           void* user_ctx);

// 链路层准入过滤器（如 FirewallClient）
class ILinkFilter {
public:
    virtual ~ILinkFilter() = default;

    virtual const char* filter_name() const noexcept {
        return "link_filter";
    }

    // 返回 true 放行，返回 false 丢弃
    virtual bool allow_frame(const uint8_t* frame, int len, PacketDirection dir, LinkType link_type,
                             const char* ifname) noexcept = 0;
};

using LinkFilterFn = bool (*)(const uint8_t* frame, int len, PacketDirection dir, LinkType link_type,
                              const char* ifname, void* user_ctx);

// L2 链路协议处理器（如 lwIP 以太网帧入口、BLE HCI Event/ACL 分发器）
class ILinkProtocolHandler {
public:
    virtual ~ILinkProtocolHandler() = default;

    virtual const char* protocol_name() const noexcept = 0;

    // channel_or_ethtype：以太网下为 EtherType（0x0800 IPv4 / 0x0806 ARP / 0 通配）；
    // BLE HCI 下为 H4 包类型（0x02 ACL / 0x04 Event / 0 通配）。
    virtual bool accepts(LinkType link_type, uint16_t channel_or_ethtype) const noexcept = 0;

    virtual ProtocolVerdict on_frame(const uint8_t* frame, int len, LinkType link_type, uint16_t channel_or_ethtype,
                                     const char* ifname) noexcept = 0;
};

class LinkLayer {
public:
    static constexpr size_t MAX_LINK_DEVICES = 4;
    static constexpr size_t MAX_LINK_TAPS = 6;
    static constexpr size_t MAX_LINK_FILTERS = 4;
    static constexpr size_t MAX_LINK_PROTOCOLS = 8;

    static LinkLayer& instance() noexcept {
        static LinkLayer layer;
        return layer;
    }

    LinkLayer() noexcept : protocol_registry_(&ProtocolRegistry::instance()) {}

    void bind_protocol_registry(ProtocolRegistry* reg) noexcept {
        protocol_registry_ = reg ? reg : &ProtocolRegistry::instance();
    }

    ProtocolRegistry* protocol_registry() const noexcept {
        return protocol_registry_;
    }

    // ========================================================================
    // 1. 链路设备注册与控制（解耦协议栈/抓包器与具体硬件网卡驱动）
    // ========================================================================
    bool register_device(const char* ifname, NetDevice* dev, LinkType type = LinkType::Ethernet) noexcept {
        if (!ifname || ifname[0] == '\0' || !dev) {
            return false;
        }
        dev->set_name(ifname);
        // 若当前已请求过全局混杂模式，新注册的以太网设备立即同步状态
        if (type == LinkType::Ethernet && promiscuous_requested_) {
            dev->set_promiscuous_mode(true);
        }

        for (size_t i = 0; i < MAX_LINK_DEVICES; ++i) {
            if (devices_[i].used && str_eq(devices_[i].ifname, ifname)) {
                devices_[i].dev = dev;
                devices_[i].type = type;
                return true;
            }
        }
        for (size_t i = 0; i < MAX_LINK_DEVICES; ++i) {
            if (!devices_[i].used) {
                devices_[i].used = true;
                copy_ifname(devices_[i].ifname, ifname);
                devices_[i].dev = dev;
                devices_[i].type = type;
                return true;
            }
        }
        return false;
    }

    bool unregister_device(const char* ifname) noexcept {
        if (!ifname) {
            return false;
        }
        bool removed = false;
        for (size_t i = 0; i < MAX_LINK_DEVICES; ++i) {
            if (devices_[i].used && str_eq(devices_[i].ifname, ifname)) {
                devices_[i] = DeviceSlot{};
                removed = true;
            }
        }
        return removed;
    }

    NetDevice* get_device(const char* ifname) const noexcept {
        if (!ifname || ifname[0] == '\0') {
            return get_default_device(LinkType::Ethernet);
        }
        for (size_t i = 0; i < MAX_LINK_DEVICES; ++i) {
            if (devices_[i].used && str_eq(devices_[i].ifname, ifname)) {
                return devices_[i].dev;
            }
        }
        return nullptr;
    }

    NetDevice* get_default_device(LinkType type = LinkType::Ethernet) const noexcept {
        for (size_t i = 0; i < MAX_LINK_DEVICES; ++i) {
            if (devices_[i].used && devices_[i].type == type && devices_[i].dev != nullptr) {
                return devices_[i].dev;
            }
        }
        return nullptr;
    }

    size_t device_count() const noexcept {
        size_t count = 0;
        for (size_t i = 0; i < MAX_LINK_DEVICES; ++i) {
            if (devices_[i].used) {
                ++count;
            }
        }
        return count;
    }

    // 统一混杂模式切换：PacketCapture 等上层组件无需引用 StellarisEth
    bool set_promiscuous_mode(bool enable, const char* ifname = nullptr) noexcept {
        if (!ifname || ifname[0] == '\0') {
            promiscuous_requested_ = enable;
        }
        bool updated = false;
        for (size_t i = 0; i < MAX_LINK_DEVICES; ++i) {
            if (!devices_[i].used || !devices_[i].dev) {
                continue;
            }
            if (!ifname || ifname[0] == '\0' || str_eq(devices_[i].ifname, ifname)) {
                devices_[i].dev->set_promiscuous_mode(enable);
                updated = true;
            }
        }
        return updated;
    }

    bool is_promiscuous_mode(const char* ifname = nullptr) const noexcept {
        for (size_t i = 0; i < MAX_LINK_DEVICES; ++i) {
            if (!devices_[i].used || !devices_[i].dev) {
                continue;
            }
            if (!ifname || ifname[0] == '\0' || str_eq(devices_[i].ifname, ifname)) {
                return devices_[i].dev->is_promiscuous_mode();
            }
        }
        return promiscuous_requested_;
    }

    // ========================================================================
    // 2. 旁路嗅探（ILinkTap）、准入过滤（ILinkFilter）、L2 协议注册
    // ========================================================================
    bool register_tap(ILinkTap* tap) noexcept {
        if (!tap) {
            return false;
        }
        LockGuard lock(mutex_);
        for (size_t i = 0; i < MAX_LINK_TAPS; ++i) {
            if (taps_[i].used && taps_[i].tap == tap) {
                return true;
            }
        }
        for (size_t i = 0; i < MAX_LINK_TAPS; ++i) {
            if (!taps_[i].used) {
                taps_[i].used = true;
                taps_[i].tap = tap;
                taps_[i].fn = nullptr;
                taps_[i].user_ctx = nullptr;
                return true;
            }
        }
        return false;
    }

    bool register_tap_fn(const char* /*name*/, LinkTapFn fn, void* user_ctx = nullptr) noexcept {
        if (!fn) {
            return false;
        }
        LockGuard lock(mutex_);
        for (size_t i = 0; i < MAX_LINK_TAPS; ++i) {
            if (taps_[i].used && taps_[i].fn == fn && taps_[i].user_ctx == user_ctx) {
                return true;
            }
        }
        for (size_t i = 0; i < MAX_LINK_TAPS; ++i) {
            if (!taps_[i].used) {
                taps_[i].used = true;
                taps_[i].tap = nullptr;
                taps_[i].fn = fn;
                taps_[i].user_ctx = user_ctx;
                return true;
            }
        }
        return false;
    }

    bool unregister_tap(const ILinkTap* tap) noexcept {
        if (!tap) {
            return false;
        }
        LockGuard lock(mutex_);
        bool removed = false;
        for (size_t i = 0; i < MAX_LINK_TAPS; ++i) {
            if (taps_[i].used && taps_[i].tap == tap) {
                taps_[i] = TapSlot{};
                removed = true;
            }
        }
        return removed;
    }

    bool register_filter(ILinkFilter* filter) noexcept {
        if (!filter) {
            return false;
        }
        LockGuard lock(mutex_);
        for (size_t i = 0; i < MAX_LINK_FILTERS; ++i) {
            if (filters_[i].used && filters_[i].filter == filter) {
                return true;
            }
        }
        for (size_t i = 0; i < MAX_LINK_FILTERS; ++i) {
            if (!filters_[i].used) {
                filters_[i].used = true;
                filters_[i].filter = filter;
                filters_[i].fn = nullptr;
                filters_[i].user_ctx = nullptr;
                return true;
            }
        }
        return false;
    }

    bool register_filter_fn(const char* /*name*/, LinkFilterFn fn, void* user_ctx = nullptr) noexcept {
        if (!fn) {
            return false;
        }
        LockGuard lock(mutex_);
        for (size_t i = 0; i < MAX_LINK_FILTERS; ++i) {
            if (filters_[i].used && filters_[i].fn == fn && filters_[i].user_ctx == user_ctx) {
                return true;
            }
        }
        for (size_t i = 0; i < MAX_LINK_FILTERS; ++i) {
            if (!filters_[i].used) {
                filters_[i].used = true;
                filters_[i].filter = nullptr;
                filters_[i].fn = fn;
                filters_[i].user_ctx = user_ctx;
                return true;
            }
        }
        return false;
    }

    bool unregister_filter(const ILinkFilter* filter) noexcept {
        if (!filter) {
            return false;
        }
        LockGuard lock(mutex_);
        bool removed = false;
        for (size_t i = 0; i < MAX_LINK_FILTERS; ++i) {
            if (filters_[i].used && filters_[i].filter == filter) {
                filters_[i] = FilterSlot{};
                removed = true;
            }
        }
        return removed;
    }

    bool register_link_protocol(ILinkProtocolHandler* handler) noexcept {
        if (!handler) {
            return false;
        }
        LockGuard lock(mutex_);
        for (size_t i = 0; i < MAX_LINK_PROTOCOLS; ++i) {
            if (link_protocols_[i] == handler) {
                return true;
            }
        }
        for (size_t i = 0; i < MAX_LINK_PROTOCOLS; ++i) {
            if (link_protocols_[i] == nullptr) {
                link_protocols_[i] = handler;
                return true;
            }
        }
        return false;
    }

    bool unregister_link_protocol(const ILinkProtocolHandler* handler) noexcept {
        if (!handler) {
            return false;
        }
        LockGuard lock(mutex_);
        bool removed = false;
        for (size_t i = 0; i < MAX_LINK_PROTOCOLS; ++i) {
            if (link_protocols_[i] == handler) {
                link_protocols_[i] = nullptr;
                removed = true;
            }
        }
        return removed;
    }

    bool has_link_protocol(const ILinkProtocolHandler* handler) const noexcept {
        if (!handler) {
            return false;
        }
        for (size_t i = 0; i < MAX_LINK_PROTOCOLS; ++i) {
            if (link_protocols_[i] == handler) {
                return true;
            }
        }
        return false;
    }

    // ========================================================================
    // 3. 统一数据帧入站与出站管线
    // ========================================================================
    void notify_taps(const uint8_t* frame, int len, PacketDirection dir, LinkType link_type,
                     const char* ifname) noexcept {
        if (!frame || len <= 0) {
            return;
        }
        const char* safe_if = (ifname && ifname[0] != '\0') ? ifname : "en0";
        // 快照后锁外回调：Tap 回调可能收发联动再入本类（非递归 Mutex），
        // 并发注销时快照保证不撕裂槽位（Tap 对象须为长命静态对象）。
        TapSlot snapshot[MAX_LINK_TAPS];
        {
            LockGuard lock(mutex_);
            for (size_t i = 0; i < MAX_LINK_TAPS; ++i) {
                snapshot[i] = taps_[i];
            }
        }
        for (size_t i = 0; i < MAX_LINK_TAPS; ++i) {
            if (!snapshot[i].used) {
                continue;
            }
            if (snapshot[i].tap) {
                snapshot[i].tap->on_frame(frame, len, dir, link_type, safe_if);
            } else if (snapshot[i].fn) {
                snapshot[i].fn(frame, len, dir, link_type, safe_if, snapshot[i].user_ctx);
            }
        }
    }

    bool check_filters(const uint8_t* frame, int len, PacketDirection dir, LinkType link_type,
                       const char* ifname) noexcept {
        if (!frame || len <= 0) {
            return false;
        }
        const char* safe_if = (ifname && ifname[0] != '\0') ? ifname : "en0";
        // 快照后锁外回调（同 notify_taps 的理由）
        FilterSlot snapshot[MAX_LINK_FILTERS];
        {
            LockGuard lock(mutex_);
            for (size_t i = 0; i < MAX_LINK_FILTERS; ++i) {
                snapshot[i] = filters_[i];
            }
        }
        for (size_t i = 0; i < MAX_LINK_FILTERS; ++i) {
            if (!snapshot[i].used) {
                continue;
            }
            bool allowed = true;
            if (snapshot[i].filter) {
                allowed = snapshot[i].filter->allow_frame(frame, len, dir, link_type, safe_if);
            } else if (snapshot[i].fn) {
                allowed = snapshot[i].fn(frame, len, dir, link_type, safe_if, snapshot[i].user_ctx);
            }
            if (!allowed) {
                return false;
            }
        }
        return true;
    }

    // 将一帧已收到的链路数据送入 LinkLayer 管线：
    //   1) 触发所有 ILinkTap 旁路嗅探（如 PacketCapture、IdsEngine）
    //   2) 执行所有 ILinkFilter 准入检查（如 Firewall）
    //   3) 若为 Ethernet 帧则上送 ProtocolRegistry 解复用 L4 协议（如 DistributedSoftBus）
    //   4) 分发给匹配的 ILinkProtocolHandler（如 lwIP、BLE HCI 分发器）
    ProtocolVerdict input_frame(const uint8_t* frame, int len, LinkType link_type = LinkType::Ethernet,
                                uint16_t channel_or_ethtype = 0, const char* ifname = "en0") noexcept {
        if (!frame || len <= 0) {
            return ProtocolVerdict::Drop;
        }
        const char* safe_if = (ifname && ifname[0] != '\0') ? ifname : "en0";

        // 1. 旁路嗅探（即使后续被防火墙丢弃，抓包器与 NIDS 仍能观测到入站原始帧）
        notify_taps(frame, len, PacketDirection::Rx, link_type, safe_if);

        // 2. 链路/防火墙过滤
        if (!check_filters(frame, len, PacketDirection::Rx, link_type, safe_if)) {
            rx_dropped_++;
            return ProtocolVerdict::Drop;
        }

        // 3. 以太网帧自动提取 EtherType 并送入 L4 ProtocolRegistry
        uint16_t resolved_channel = channel_or_ethtype;
        if (link_type == LinkType::Ethernet) {
            if (resolved_channel == 0 && len >= 14) {
                resolved_channel = static_cast<uint16_t>((static_cast<uint16_t>(frame[12]) << 8) | frame[13]);
            }
            if (protocol_registry_) {
                const ProtocolVerdict l4_verdict =
                    protocol_registry_->dispatch_ethernet_frame(frame, len, PacketDirection::Rx, safe_if);
                if (l4_verdict == ProtocolVerdict::Drop) {
                    rx_dropped_++;
                    return ProtocolVerdict::Drop;
                }
                if (l4_verdict == ProtocolVerdict::Consume) {
                    rx_frames_++;
                    return ProtocolVerdict::Consume;
                }
            }
        }

        // 4. 分发到注册的 L2 链路协议处理器（快照后锁外回调，理由同上）
        ProtocolVerdict final_verdict = ProtocolVerdict::Pass;
        ILinkProtocolHandler* protos[MAX_LINK_PROTOCOLS];
        {
            LockGuard lock(mutex_);
            for (size_t i = 0; i < MAX_LINK_PROTOCOLS; ++i) {
                protos[i] = link_protocols_[i];
            }
        }
        for (size_t i = 0; i < MAX_LINK_PROTOCOLS; ++i) {
            ILinkProtocolHandler* handler = protos[i];
            if (!handler || !handler->accepts(link_type, resolved_channel)) {
                continue;
            }
            const ProtocolVerdict v = handler->on_frame(frame, len, link_type, resolved_channel, safe_if);
            if (v == ProtocolVerdict::Drop) {
                rx_dropped_++;
                return ProtocolVerdict::Drop;
            }
            if (v == ProtocolVerdict::Consume) {
                final_verdict = ProtocolVerdict::Consume;
                break;
            }
        }

        rx_frames_++;
        return final_verdict;
    }

    // 从指定网卡驱动拉取一帧并执行入站管线；
    // 若被防火墙丢弃或被高优独占协议 Consume，则返回 0，否则返回供上层协议栈（如 lwIP）消费的字节数。
    int receive_frame(const char* ifname, uint8_t* buffer, int max_len, NetDevice* fallback_dev = nullptr) noexcept {
        if (!buffer || max_len <= 0) {
            return 0;
        }
        NetDevice* dev = get_device(ifname);
        if (!dev) {
            dev = fallback_dev;
        }
        if (!dev) {
            return 0;
        }

        const int bytes_read = dev->receive_frame(buffer, max_len);
        if (bytes_read <= 0) {
            return 0;
        }

        const char* safe_if = (ifname && ifname[0] != '\0') ? ifname : dev->get_name();
        const ProtocolVerdict verdict = input_frame(buffer, bytes_read, LinkType::Ethernet, 0, safe_if);
        if (verdict == ProtocolVerdict::Drop || verdict == ProtocolVerdict::Consume) {
            return 0;
        }
        return bytes_read;
    }

    // 通过指定网卡驱动发送一帧，自动执行出站旁路嗅探、防火墙过滤与 L4 出站审计
    bool transmit_frame(const char* ifname, const uint8_t* frame, int len, NetDevice* fallback_dev = nullptr) noexcept {
        if (!frame || len <= 0) {
            return false;
        }
        NetDevice* dev = get_device(ifname);
        if (!dev) {
            dev = fallback_dev;
        }
        const char* safe_if = (ifname && ifname[0] != '\0') ? ifname : (dev ? dev->get_name() : "en0");

        // 1. 出站旁路嗅探
        notify_taps(frame, len, PacketDirection::Tx, LinkType::Ethernet, safe_if);

        // 2. 出站准入过滤
        if (!check_filters(frame, len, PacketDirection::Tx, LinkType::Ethernet, safe_if)) {
            tx_dropped_++;
            return false;
        }

        // 3. L4 出站通知
        if (protocol_registry_) {
            const ProtocolVerdict l4_verdict =
                protocol_registry_->dispatch_ethernet_frame(frame, len, PacketDirection::Tx, safe_if);
            if (l4_verdict == ProtocolVerdict::Drop) {
                tx_dropped_++;
                return false;
            }
        }

        if (!dev) {
            tx_dropped_++;
            return false;
        }
        if (!dev->send_frame(frame, len)) {
            tx_dropped_++;
            return false;
        }
        tx_frames_++;
        return true;
    }

    uint32_t rx_frames() const noexcept {
        return rx_frames_;
    }

    uint32_t tx_frames() const noexcept {
        return tx_frames_;
    }

    uint32_t rx_dropped() const noexcept {
        return rx_dropped_;
    }

    uint32_t tx_dropped() const noexcept {
        return tx_dropped_;
    }

    void reset_stats() noexcept {
        rx_frames_ = 0;
        tx_frames_ = 0;
        rx_dropped_ = 0;
        tx_dropped_ = 0;
    }

    void reset() noexcept {
        for (size_t i = 0; i < MAX_LINK_DEVICES; ++i) {
            devices_[i] = DeviceSlot{};
        }
        for (size_t i = 0; i < MAX_LINK_TAPS; ++i) {
            taps_[i] = TapSlot{};
        }
        for (size_t i = 0; i < MAX_LINK_FILTERS; ++i) {
            filters_[i] = FilterSlot{};
        }
        for (size_t i = 0; i < MAX_LINK_PROTOCOLS; ++i) {
            link_protocols_[i] = nullptr;
        }
        promiscuous_requested_ = false;
        protocol_registry_ = &ProtocolRegistry::instance();
        reset_stats();
    }

private:
    struct DeviceSlot {
        bool used{false};
        char ifname[8]{};
        NetDevice* dev{nullptr};
        LinkType type{LinkType::Ethernet};
    };

    struct TapSlot {
        bool used{false};
        ILinkTap* tap{nullptr};
        LinkTapFn fn{nullptr};
        void* user_ctx{nullptr};
    };

    struct FilterSlot {
        bool used{false};
        ILinkFilter* filter{nullptr};
        LinkFilterFn fn{nullptr};
        void* user_ctx{nullptr};
    };

    DeviceSlot devices_[MAX_LINK_DEVICES]{};
    TapSlot taps_[MAX_LINK_TAPS]{};
    FilterSlot filters_[MAX_LINK_FILTERS]{};
    ILinkProtocolHandler* link_protocols_[MAX_LINK_PROTOCOLS]{};
    // 注册/注销/分发共用互斥（与 DeviceRouteTable/ProtocolRegistry 对齐）。
    // 回调一律快照后锁外调用（回调可能收发联动再入本类）。
    mutable Mutex mutex_;
    ProtocolRegistry* protocol_registry_{nullptr};
    bool promiscuous_requested_{false};

    uint32_t rx_frames_{0};
    uint32_t tx_frames_{0};
    uint32_t rx_dropped_{0};
    uint32_t tx_dropped_{0};

    static bool str_eq(const char* a, const char* b) noexcept {
        if (!a || !b) {
            return false;
        }
        while (*a && *b) {
            if (*a != *b) {
                return false;
            }
            ++a;
            ++b;
        }
        return *a == *b;
    }

    static void copy_ifname(char dst[8], const char* src) noexcept {
        size_t i = 0;
        if (src) {
            while (i < 7 && src[i] != '\0') {
                dst[i] = src[i];
                ++i;
            }
        }
        dst[i] = '\0';
    }
};

} // namespace net
} // namespace auroraos

#endif // AURORA_LINK_LAYER_HPP
