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

} // namespace hci
} // namespace ble
} // namespace auroraos

#endif // AURORA_HCI_EVENT_DISPATCH_HPP
