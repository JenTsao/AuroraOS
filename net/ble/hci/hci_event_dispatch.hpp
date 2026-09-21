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

// ========================================================
// HCI 事件分发器 (Header-Only)
//
// 将 Controller 上报的 HCI Event 解析后，分发到：
//   - BleScanner     (设备发现 / 指纹 / RSSI)
//   - BleIds         (入侵检测规则引擎)
//   - BleMitmDetector (MITM 攻击检测)
//   - GattAuditor    (GATT 安全审计)
//   - HalBle 连接句柄状态
//
// 这是「安全模块 ↔ 真实硬件驱动」之间的桥梁：底层 HCI
// transport (UART H4 / 内存 IPC) 解析出完整 HCI Event 后，
// 调用 dispatch_hci_event() 完成向安全模块的投递。
//
// 所有分发路径均为零堆分配、无阻塞；可在 HCI 接收线程或
// 中断下半部 (deferred worker) 中调用。
// ========================================================
namespace auroraos {
namespace ble {
namespace hci {

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

// 弱符号通知钩子（解耦 stable 与 experimental）
extern "C" __attribute__((weak)) void aurora_ble_on_connection_change(bool connected, uint16_t handle);
extern "C" __attribute__((weak)) void aurora_ble_on_data_received(uint16_t handle, const uint8_t* data, size_t len);

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
        BleScanner::instance().process_advertisement(entry.addr,
                                                     static_cast<BleAddrType>(entry.addr_type), entry.rssi,
                                                     entry.data, entry.data_len);
        // 2) IDS：广告事件 (扫描风暴 / 弱安全检测)
        uint8_t ad_flags = 0;
        if (entry.data_len >= 3 && entry.data[0] >= 2 && entry.data[1] == 0x01)
            ad_flags = entry.data[2];
        BleIds::instance().feed_advertisement(entry.addr, entry.rssi, ad_flags);
    }
}

// 分发一条完整 HCI Event 包。
// event_data 指向 Event Code 之后的数据；实际上约定调用方传入
// 完整事件包（含 Event Code 作为首字节），与 HciTransport::on_hardware_rx
// 收到的 rx_buffer 布局一致。
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

// 分发一条完整 HCI ACL 数据包
// pkt 布局：[Handle (12b) + PB (2b) + BC (2b): 2B] [Total Length: 2B] [L2CAP Header (4B)] [L2CAP Payload...]
static inline void dispatch_hci_acl(const uint8_t* pkt, size_t len) {
    if (!pkt || len < 8)
        return;

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

    // 1) CID 0x0004: ATT (Attribute Protocol)
    if (cid == 0x0004 && data_len >= 1) {
        uint8_t opcode = l2cap_data[0];

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
    }
    // 2) CID 0x0006: SMP (Security Manager Protocol)
    else if (cid == 0x0006 && data_len >= 1) {
        uint8_t smp_code = l2cap_data[0];

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
    }
}

} // namespace hci
} // namespace ble
} // namespace auroraos

#endif // AURORA_HCI_EVENT_DISPATCH_HPP
