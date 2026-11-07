#ifndef AURORA_HCI_EVENT_DISPATCH_HPP
#define AURORA_HCI_EVENT_DISPATCH_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "hci_packet.hpp"
#include "../hal_ble_impl.hpp"
#include "../ble_scanner.hpp"
#include "../ble_ids.hpp"
#include "../ble_mitm.hpp"
#include "../gatt_auditor.hpp"
#include "../../link_layer.hpp"
#include "../../protocol_registry.hpp"

// ========================================================
// HCI 事件与 BLE L2CAP 协议分发器 (Header-Only)
//
// 通过 LinkLayer (LinkType::BleHci) 与 ProtocolRegistry (PROTO_BLE_L2CAP)
// 将 BLE HCI Event、ATT (CID 0x0004)、SMP (CID 0x0006) 注册为可插拔链路/协议处理器，
// 消除底层 HCI UART 驱动对上层 GATT/SMP/安全审计协议的直接硬编码穿透。
// ========================================================
namespace auroraos {
namespace ble {
namespace hci {

static constexpr uint16_t L2CAP_CID_ATT = 0x0004;
static constexpr uint16_t L2CAP_CID_SMP = 0x0006;

// 连接句柄 → 对端 MAC 映射由 hal_ble_impl.cpp 统一维护（单例状态），
// 避免 header-only 内部链接拷贝导致各编译单元状态不一致。
static inline void remember_conn_mac(uint16_t handle, const uint8_t mac[6]) {
    hal_ble_remember_conn_mac(handle, mac);
}

static inline bool lookup_conn_mac(uint16_t handle, uint8_t mac_out[6]) {
    return hal_ble_take_conn_mac(handle, mac_out);
}

// 非破坏性查询：仅读取 handle 对应的 MAC，不删除映射。
// 供 BleManager 在签名验证失败等路径向 IDS 上报时反查对端 MAC。
static inline bool query_conn_mac(uint16_t handle, uint8_t mac_out[6]) {
    return hal_ble_query_conn_mac(handle, mac_out);
}

// 弱符号通知钩子（解耦 stable 与 experimental / NimBLE 适配层）
extern "C" __attribute__((weak)) void aurora_ble_on_connection_change(bool connected, uint16_t handle);
extern "C" __attribute__((weak)) void aurora_ble_on_data_received(uint16_t handle, const uint8_t* data, size_t len);
extern "C" __attribute__((weak)) void aurora_nimble_rx_event(const uint8_t* data, size_t len);
extern "C" __attribute__((weak)) void aurora_nimble_rx_acl(const uint8_t* data, size_t len);

inline void ensure_ble_protocols_registered() noexcept;

// 分发 LE Connection Complete 事件 (Vol 4 Part E §7.7.65.1)
static inline void dispatch_le_connection_complete(const uint8_t* p, size_t len) {
    LeConnectionComplete evt{};
    if (!parse_le_connection_complete(p, len, evt))
        return;
    if (evt.status != 0x00)
        return; // 连接建立失败，忽略

    remember_conn_mac(evt.connection_handle, evt.peer_addr);
    hal_ble_set_conn_handle(evt.connection_handle);

    // 1) MITM 检测：记录连接参数
    BleConnParams params{};
    params.interval_min = evt.interval;
    params.interval_max = evt.interval;
    params.latency = evt.latency;
    params.supervision_to = evt.supervision_to;
    params.hop_increment = 0; // LE Connection Complete 不携带 hop，置 0
    BleMitmDetector::instance().on_connect(evt.peer_addr, params, /*rssi=*/0, /*le_secure=*/false);

    // 2) GATT 审计：注册设备
    GattAuditor::instance().register_device(evt.peer_addr);

    // 3) IDS：连接事件
    BleIds::instance().feed_connection_event(evt.peer_addr, /*is_connect=*/true, /*reason=*/0);

    // 4) 通知上层状态机
    if (aurora_ble_on_connection_change) {
        aurora_ble_on_connection_change(true, evt.connection_handle);
    }
}

// 分发 Disconnection Complete 事件 (Vol 2 Part E §7.7.5)
static inline void dispatch_disconnection_complete(const uint8_t* p, size_t len) {
    DisconnectionComplete evt{};
    if (!parse_disconnection_complete(p, len, evt))
        return;

    hal_ble_set_conn_handle(0xFFFF);

    uint8_t mac[6];
    if (lookup_conn_mac(evt.connection_handle, mac)) {
        BleMitmDetector::instance().on_disconnect(mac, evt.reason);
        GattAuditor::instance().unregister_device(mac);
        BleIds::instance().feed_connection_event(mac, /*is_connect=*/false, evt.reason);
    }

    // 通知上层状态机
    if (aurora_ble_on_connection_change) {
        aurora_ble_on_connection_change(false, evt.connection_handle);
    }
}

// 分发 LE Advertising Report 事件 (Vol 4 Part E §7.7.65.2)
static inline void dispatch_le_advertising_report(const uint8_t* p, size_t len) {
    if (!p || len < 1)
        return;
    uint8_t num_reports = p[0];
    size_t off = 1;
    for (uint8_t i = 0; i < num_reports; ++i) {
        LeAdvertisingReportEntry entry{};
        size_t consumed = parse_le_advertising_report_entry(p + off, len - off, entry);
        if (consumed == 0)
            break;
        off += consumed;

        // 1) 扫描器：指纹 + RSSI 跟踪
        BleScanner::instance().process_advertisement(entry.addr, static_cast<BleAddrType>(entry.addr_type), entry.rssi,
                                                     entry.data, entry.data_len);
        // 2) IDS：广告事件 (扫描风暴 / 弱安全检测)
        uint8_t ad_flags = 0;
        if (entry.data_len >= 3 && entry.data[0] >= 2 && entry.data[1] == 0x01)
            ad_flags = entry.data[2];
        BleIds::instance().feed_advertisement(entry.addr, entry.rssi, ad_flags);
    }
}

// 分发一条完整 HCI Event 包。
static inline void dispatch_hci_event(const uint8_t* pkt, size_t len) {
    if (!pkt || len < 2)
        return;

    uint8_t event_code = pkt[0];
    const uint8_t* params = pkt + 2; // 跳过 Event Code + Param Len
    size_t params_len = len - 2;

    switch (event_code) {
    case EVT_DISCONNECTION_COMPLETE:
        dispatch_disconnection_complete(params, params_len);
        break;
    case EVT_LE_META:
        if (params_len < 1)
            return;
        switch (params[0]) { // subevent code
        case LE_SUB_CONNECTION_COMPLETE:
            dispatch_le_connection_complete(params + 1, params_len - 1);
            break;
        case LE_SUB_ADVERTISING_REPORT:
            dispatch_le_advertising_report(params + 1, params_len - 1);
            break;
        default:
            break; // 其余子事件暂不进入安全模块
        }
        break;
    default:
        break;
    }
}

// ========================================================
// BLE ATT (L2CAP CID 0x0004) 可插拔 L4 协议处理器
// ========================================================
class BleAttProtocolHandler final : public auroraos::net::IProtocolHandler {
public:
    const char* name() const noexcept override {
        return "ble_att";
    }

    auroraos::net::ProtocolVerdict on_packet(const auroraos::net::L4PacketContext& ctx, const uint8_t* l2cap_data,
                                             size_t data_len) noexcept override {
        if (!l2cap_data || data_len < 1) {
            return auroraos::net::ProtocolVerdict::Pass;
        }

        const uint16_t handle = ctx.src_port;
        const uint8_t* mac = ctx.src_mac;
        const uint8_t opcode = l2cap_data[0];

        // 1.1 GATT 发现风暴审计 (Read by Group Type / Read by Type / Find Info)
        if (opcode == 0x08 || opcode == 0x10 || opcode == 0x04) {
            BleIdsEvent evt{};
            evt.timestamp = tick_count;
            memcpy(evt.mac, mac, 6);
            evt.event_type = BleIdsEventType::GattDiscoveryStorm;
            evt.severity = 30;
            evt.param = opcode;
            BleIds::instance().push_event(evt);
        }
        // 1.2 特征值写入安全审计 (Write Request 0x12 / Write Command 0x52)
        else if (opcode == 0x12 || opcode == 0x52) {
            if (data_len >= 3) {
                uint16_t att_handle = static_cast<uint16_t>(l2cap_data[1] | (l2cap_data[2] << 8));
                const uint8_t* val = l2cap_data + 3;
                size_t val_len = data_len - 3;

                // 通知上层数据到达 (如 Lua 签名包或控制包)
                if (aurora_ble_on_data_received) {
                    aurora_ble_on_data_received(handle, val, val_len);
                }

                GattFinding finding{};
                finding.timestamp = tick_count;
                memcpy(finding.device_mac, mac, 6);
                finding.char_handle = att_handle;
                finding.char_uuid16 = 0;
                finding.type = GattFindingType::UnauthenticatedWrite;
                finding.severity = GattAuditSeverity::Warning;
                strncpy(finding.description, "GATT write on handle", sizeof(finding.description) - 1);
            }
        }
        // 1.3 发现响应/特征值信息解析 (Read by Type Response 0x09: Characteristic Declaration)
        else if (opcode == 0x09 && data_len >= 2) {
            uint8_t item_len = l2cap_data[1];
            if (item_len >= 7 && (data_len - 2) >= item_len) {
                // 每个条目: [Handle: 2B] [Properties: 1B] [ValueHandle: 2B] [UUID16: 2B]
                size_t off = 2;
                while (off + item_len <= data_len) {
                    uint16_t char_handle = static_cast<uint16_t>(l2cap_data[off + 3] | (l2cap_data[off + 4] << 8));
                    uint8_t props = l2cap_data[off + 2];
                    uint16_t uuid16 = static_cast<uint16_t>(l2cap_data[off + 5] | (l2cap_data[off + 6] << 8));

                    GattCharInfo ch{};
                    ch.handle = char_handle;
                    ch.uuid16 = uuid16;
                    ch.properties = props;
                    ch.security_level = (props & (GattProp::AuthSigned)) ? 2 : 0;
                    ch.requires_auth = (props & (GattProp::AuthSigned)) != 0;
                    ch.requires_encryption = false;

                    // 注册并自动审计发现的特征
                    GattAuditor::instance().add_characteristic(mac, ch);
                    off += item_len;
                }
            }
        }
        return auroraos::net::ProtocolVerdict::Pass;
    }
};

// ========================================================
// BLE SMP (L2CAP CID 0x0006) 可插拔 L4 协议处理器
// ========================================================
class BleSmpProtocolHandler final : public auroraos::net::IProtocolHandler {
public:
    const char* name() const noexcept override {
        return "ble_smp";
    }

    auroraos::net::ProtocolVerdict on_packet(const auroraos::net::L4PacketContext& ctx, const uint8_t* l2cap_data,
                                             size_t data_len) noexcept override {
        if (!l2cap_data || data_len < 1) {
            return auroraos::net::ProtocolVerdict::Pass;
        }

        const uint8_t* mac = ctx.src_mac;
        const uint8_t smp_code = l2cap_data[0];

        // 2.1 Pairing Request (0x01) / Pairing Response (0x02)
        // 格式: [Code: 1B] [IOCap: 1B] [OOB: 1B] [AuthReq: 1B] [MaxKeySize: 1B] [InitKeyDist: 1B] [RespKeyDist: 1B]
        if ((smp_code == 0x01 || smp_code == 0x02) && data_len >= 7) {
            uint8_t oob = l2cap_data[2];
            uint8_t auth_req = l2cap_data[3];
            uint8_t max_key_size = l2cap_data[4];

            bool le_sc = (auth_req & 0x08) != 0; // Bit 3: SC (LE Secure Connections)
            bool mitm = (auth_req & 0x04) != 0;  // Bit 2: MITM protection
            bool has_oob = (oob == 0x01);

            BlePairingMethod method = BlePairingMethod::JustWorks;
            if (has_oob) {
                method = BlePairingMethod::Oob;
            } else if (le_sc && mitm) {
                method = BlePairingMethod::NumericComp;
            } else if (mitm) {
                method = BlePairingMethod::PasskeyEntry;
            }

            // 通知 MITM 引擎记录配对信息与检测降级
            uint8_t sec_level = (le_sc && mitm) ? 4 : (mitm ? 3 : (le_sc ? 2 : 1));
            BleMitmDetector::instance().on_pairing_complete(mac, method, sec_level, le_sc, has_oob);

            // 若请求弱配对 (无 LE SC 或无 MITM 保护或秘钥长度不足)，记录弱安全事件
            if (!le_sc || !mitm || max_key_size < 16) {
                BleIdsEvent evt{};
                evt.timestamp = tick_count;
                memcpy(evt.mac, mac, 6);
                evt.event_type = BleIdsEventType::WeakSecurity;
                evt.severity = (!le_sc) ? 70 : 50;
                evt.param = (static_cast<uint16_t>(auth_req) << 8) | max_key_size;
                BleIds::instance().push_event(evt);
            }
        }
        // 2.2 Pairing Failed (0x05)
        else if (smp_code == 0x05 && data_len >= 2) {
            uint8_t reason = l2cap_data[1];
            (void)reason;
            BleMitmDetector::instance().on_pairing_complete(mac, BlePairingMethod::JustWorks, 0, false, false);
        }
        return auroraos::net::ProtocolVerdict::Pass;
    }
};

// 分发一条完整 HCI ACL 数据包（解析 L2CAP 帧头后交由 ProtocolRegistry L4 分发）
// pkt 布局：[Handle (12b) + PB (2b) + BC (2b): 2B] [Total Length: 2B] [L2CAP Header (4B)] [L2CAP Payload...]
static inline void dispatch_hci_acl(const uint8_t* pkt, size_t len) {
    if (!pkt || len < 8)
        return;

    ensure_ble_protocols_registered();

    uint16_t handle = static_cast<uint16_t>(pkt[0] | ((pkt[1] & 0x0F) << 8));
    uint16_t acl_len = static_cast<uint16_t>(pkt[2] | (pkt[3] << 8));
    if (len < static_cast<size_t>(4 + acl_len) || acl_len < 4)
        return;

    // L2CAP 基本帧头 (Core Spec Vol 3 Part A §3.1)
    uint16_t l2cap_len = static_cast<uint16_t>(pkt[4] | (pkt[5] << 8));
    uint16_t cid = static_cast<uint16_t>(pkt[6] | (pkt[7] << 8));

    if (acl_len < 4 + l2cap_len)
        return;

    const uint8_t* l2cap_data = pkt + 8;
    size_t data_len = l2cap_len;

    uint8_t mac[6] = {0};
    query_conn_mac(handle, mac);

    auroraos::net::L4PacketContext ctx{};
    ctx.dir = auroraos::net::PacketDirection::Rx;
    ctx.ip_proto = auroraos::net::PROTO_BLE_L2CAP;
    memcpy(ctx.src_mac, mac, 6);
    ctx.src_port = handle;
    ctx.dst_port = cid;
    ctx.raw_frame = pkt;
    ctx.raw_frame_len = static_cast<uint16_t>(len);
    ctx.ifname = "ble0";

    (void)auroraos::net::ProtocolRegistry::instance().dispatch_l4(ctx, l2cap_data, data_len);
}

// ========================================================
// BLE HCI 链路层处理器：注册于 LinkLayer (LinkType::BleHci)
// ========================================================
class BleHciLinkHandler final : public auroraos::net::ILinkProtocolHandler {
public:
    const char* protocol_name() const noexcept override {
        return "ble_hci";
    }

    bool accepts(auroraos::net::LinkType link_type, uint16_t channel_or_ethtype) const noexcept override {
        (void)channel_or_ethtype;
        return link_type == auroraos::net::LinkType::BleHci;
    }

    auroraos::net::ProtocolVerdict on_frame(const uint8_t* frame, int len, auroraos::net::LinkType link_type,
                                            uint16_t channel_or_ethtype, const char* ifname) noexcept override {
        (void)ifname;
        if (link_type != auroraos::net::LinkType::BleHci || !frame || len <= 0) {
            return auroraos::net::ProtocolVerdict::Pass;
        }

        const size_t frame_len = static_cast<size_t>(len);
        if (channel_or_ethtype == 0x04) {
            dispatch_hci_event(frame, frame_len);
            if (aurora_nimble_rx_event) {
                aurora_nimble_rx_event(frame, frame_len);
            }
            return auroraos::net::ProtocolVerdict::Consume;
        }

        if (channel_or_ethtype == 0x02) {
            dispatch_hci_acl(frame, frame_len);
            if (aurora_nimble_rx_acl) {
                aurora_nimble_rx_acl(frame, frame_len);
            }
            return auroraos::net::ProtocolVerdict::Consume;
        }

        return auroraos::net::ProtocolVerdict::Pass;
    }
};

// 幂等注册 BLE HCI 链路处理器与 ATT/SMP L4 协议处理器
inline void ensure_ble_protocols_registered() noexcept {
    static BleHciLinkHandler s_hci_link_handler;
    static BleAttProtocolHandler s_att_handler;
    static BleSmpProtocolHandler s_smp_handler;

    auroraos::net::LinkLayer::instance().register_link_protocol(&s_hci_link_handler);

    auto& reg = auroraos::net::ProtocolRegistry::instance();
    reg.register_handler(auroraos::net::PROTO_BLE_L2CAP, L2CAP_CID_ATT, &s_att_handler,
                         auroraos::net::ProtocolRegistry::PRIORITY_SERVICE);
    reg.register_handler(auroraos::net::PROTO_BLE_L2CAP, L2CAP_CID_SMP, &s_smp_handler,
                         auroraos::net::ProtocolRegistry::PRIORITY_SERVICE);
}

} // namespace hci
} // namespace ble
} // namespace auroraos

#endif // AURORA_HCI_EVENT_DISPATCH_HPP
