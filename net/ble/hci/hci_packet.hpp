#ifndef AURORA_HCI_PACKET_HPP
#define AURORA_HCI_PACKET_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// ========================================================
// HCI 包编解码层 (Header-Only, 零堆分配)
//
// 将 HalBle 抽象调用映射为真实的 HCI Command 字节流（下行），
// 并将 Controller 上报的 HCI Event 解析为结构化事件（上行），
// 供安全模块 (BleScanner / BleIds / BleMitmDetector / GattAuditor)
// 与 BleManager 状态机消费。
//
// 参考：Bluetooth Core Spec v5.3
//   Vol 2 Part E §5.4  HCI Command / Event 包格式
//   Vol 4 Part E §7.8  LE Controller Commands
//   Vol 4 Part E §7.7  LE Meta Event
// ========================================================
namespace auroraos {
namespace ble {
namespace hci {

// ---- H4 包类型 (Vol 2 Part E §5.4.1) ----
static constexpr uint8_t PKT_TYPE_COMMAND = 0x01;
static constexpr uint8_t PKT_TYPE_ACL = 0x02;
static constexpr uint8_t PKT_TYPE_SCO = 0x03;
static constexpr uint8_t PKT_TYPE_EVENT = 0x04;

// ---- HCI Event Code ----
static constexpr uint8_t EVT_DISCONNECTION_COMPLETE = 0x05;
static constexpr uint8_t EVT_COMMAND_COMPLETE = 0x0E;
static constexpr uint8_t EVT_COMMAND_STATUS = 0x0F;
static constexpr uint8_t EVT_LE_META = 0x3E;

// ---- LE Meta 子事件 (Vol 4 Part E §7.7.65) ----
static constexpr uint8_t LE_SUB_CONNECTION_COMPLETE = 0x01;
static constexpr uint8_t LE_SUB_ADVERTISING_REPORT = 0x02;
static constexpr uint8_t LE_SUB_CONNECTION_UPDATE_COMPLETE = 0x03;
static constexpr uint8_t LE_SUB_READ_REMOTE_FEATURES_COMPLETE = 0x04;
static constexpr uint8_t LE_SUB_LTK_REQUEST = 0x05;
static constexpr uint8_t LE_SUB_REMOTE_CONN_PARAM_REQUEST = 0x06;
static constexpr uint8_t LE_SUB_DATA_LENGTH_CHANGE = 0x07;
static constexpr uint8_t LE_SUB_PHY_UPDATE_COMPLETE = 0x0C;

// ---- OpCode = OGF(6bit) << 10 | OCF(10bit) ----
namespace Ogf {
static constexpr uint8_t LinkControl = 0x01;
static constexpr uint8_t LinkPolicy = 0x02;
static constexpr uint8_t ControllerBaseband = 0x03;
static constexpr uint8_t LeController = 0x08;
} // namespace Ogf

static inline constexpr uint16_t opcode(uint8_t ogf, uint16_t ocf) {
    return static_cast<uint16_t>((static_cast<uint16_t>(ogf) << 10) | (ocf & 0x03FF));
}

namespace Op {
// Controller & Baseband
static constexpr uint16_t Reset = 0x0C03;                    // OGF 0x03 OCF 0x0003
static constexpr uint16_t ReadLocalVersionInfo = 0x1001;     // OGF 0x04 OCF 0x0001
// Link Control
static constexpr uint16_t Disconnect = 0x0406;               // OGF 0x01 OCF 0x0006
// LE Controller (OGF 0x08)
static constexpr uint16_t LeSetAdvertisingParameters = 0x2006; // OCF 0x0006
static constexpr uint16_t LeSetAdvertisingData = 0x2008;       // OCF 0x0008
static constexpr uint16_t LeSetScanResponseData = 0x2009;      // OCF 0x0009
static constexpr uint16_t LeSetAdvertiseEnable = 0x200A;       // OCF 0x000A
static constexpr uint16_t LeSetScanParameters = 0x200B;        // OCF 0x000B
static constexpr uint16_t LeSetScanEnable = 0x200C;            // OCF 0x000C
} // namespace Op

// ---- 常量上限 ----
static constexpr size_t HCI_CMD_MAX_LEN = 32;   // LE Set Advertising Data 需要 32 字节 payload
static constexpr uint8_t BLE_ADV_DATA_MAX = 31; // 广告数据上限

// ========================================================
// 下行命令构建：把命令写入 out 缓冲区，返回写入长度。
// 调用方需提供 ≥ HCI_CMD_MAX_LEN 的缓冲区。
// 统一格式：OpCode(2, LE) + ParamLen(1) + Params。
// ========================================================

// HCI Reset
static inline size_t build_reset_cmd(uint8_t* out) {
    out[0] = static_cast<uint8_t>(Op::Reset & 0xFF);
    out[1] = static_cast<uint8_t>(Op::Reset >> 8);
    out[2] = 0x00;
    return 3;
}

// LE Set Advertise Enable (Vol 4 Part E §7.8.9)
static inline size_t build_le_set_advertise_enable(uint8_t* out, bool enable) {
    out[0] = static_cast<uint8_t>(Op::LeSetAdvertiseEnable & 0xFF);
    out[1] = static_cast<uint8_t>(Op::LeSetAdvertiseEnable >> 8);
    out[2] = 0x01;
    out[3] = enable ? 0x01 : 0x00;
    return 4;
}

// LE Set Advertising Data (Vol 4 Part E §7.8.7)
// ad_data: 已构建的完整 AD payload (≤31 字节，不含长度前缀)
static inline size_t build_le_set_advertising_data(uint8_t* out, const uint8_t* ad_data, size_t ad_len) {
    if (ad_len > BLE_ADV_DATA_MAX)
        ad_len = BLE_ADV_DATA_MAX;
    out[0] = static_cast<uint8_t>(Op::LeSetAdvertisingData & 0xFF);
    out[1] = static_cast<uint8_t>(Op::LeSetAdvertisingData >> 8);
    out[2] = 0x20; // Parameter length = 1 (len byte) + 31 (data)
    out[3] = static_cast<uint8_t>(ad_len);
    memset(&out[4], 0, BLE_ADV_DATA_MAX);
    if (ad_data && ad_len > 0)
        memcpy(&out[4], ad_data, ad_len);
    return 4 + BLE_ADV_DATA_MAX;
}

// LE Set Advertising Parameters (Vol 4 Part E §7.8.5)
// 用于广播模式：可连接、非定向、通用可发现。
static inline size_t build_le_set_advertising_parameters(uint8_t* out, bool connectable) {
    out[0] = static_cast<uint8_t>(Op::LeSetAdvertisingParameters & 0xFF);
    out[1] = static_cast<uint8_t>(Op::LeSetAdvertisingParameters >> 8);
    out[2] = 0x0F; // param len = 15
    size_t p = 3;
    out[p++] = 0x80; // adv_interval_min = 0x0080 * 0.625ms ≈ 80ms
    out[p++] = 0x00;
    out[p++] = 0xA0; // adv_interval_max = 0x00A0 * 0.625ms ≈ 100ms
    out[p++] = 0x00;
    out[p++] = 0x00; // adv_type: ADV_IND (connectable undirected)
    out[p++] = 0x00; // own_addr_type: public
    out[p++] = 0x00; // peer_addr_type: public
    for (int i = 0; i < 6; ++i)
        out[p++] = 0x00; // peer_addr
    out[p++] = 0x07; // adv_channel_map: ch 37/38/39
    out[p++] = 0x00; // filter policy: accept all
    (void)connectable;
    return p;
}

// LE Set Scan Parameters (Vol 4 Part E §7.8.10) — 用于被动扫描
static inline size_t build_le_set_scan_parameters(uint8_t* out) {
    out[0] = static_cast<uint8_t>(Op::LeSetScanParameters & 0xFF);
    out[1] = static_cast<uint8_t>(Op::LeSetScanParameters >> 8);
    out[2] = 0x07;
    size_t p = 3;
    out[p++] = 0x01; // scan_type: active
    out[p++] = 0x10; // scan_interval = 0x0010 * 0.625ms = 10ms
    out[p++] = 0x00;
    out[p++] = 0x10; // scan_window = 10ms
    out[p++] = 0x00;
    out[p++] = 0x00; // own_addr_type: public
    out[p++] = 0x00; // filter policy: accept all
    return p;
}

// LE Set Scan Enable (Vol 4 Part E §7.8.11)
static inline size_t build_le_set_scan_enable(uint8_t* out, bool enable, bool filter_duplicates) {
    out[0] = static_cast<uint8_t>(Op::LeSetScanEnable & 0xFF);
    out[1] = static_cast<uint8_t>(Op::LeSetScanEnable >> 8);
    out[2] = 0x02;
    out[3] = enable ? 0x01 : 0x00;
    out[4] = filter_duplicates ? 0x01 : 0x00;
    return 5;
}

// Disconnect (Vol 2 Part E §7.1.6)
static inline size_t build_disconnect_cmd(uint8_t* out, uint16_t connection_handle, uint8_t reason) {
    out[0] = static_cast<uint8_t>(Op::Disconnect & 0xFF);
    out[1] = static_cast<uint8_t>(Op::Disconnect >> 8);
    out[2] = 0x03;
    out[3] = static_cast<uint8_t>(connection_handle & 0xFF);
    out[4] = static_cast<uint8_t>(connection_handle >> 8);
    out[5] = reason;
    return 6;
}

// ========================================================
// 上行事件解析
// ========================================================

// LE 连接完成事件参数 (Vol 4 Part E §7.7.65.1)
struct LeConnectionComplete {
    uint8_t status;
    uint16_t connection_handle;
    uint8_t role;             // 0=master, 1=slave
    uint8_t peer_addr_type;
    uint8_t peer_addr[6];
    uint16_t interval;        // *1.25ms
    uint16_t latency;
    uint16_t supervision_to;  // *10ms
};

// 断开完成事件参数 (Vol 2 Part E §7.7.5)
struct DisconnectionComplete {
    uint8_t status;
    uint16_t connection_handle;
    uint8_t reason;
};

// 广告报告单条 (Vol 4 Part E §7.7.65.2)
struct LeAdvertisingReportEntry {
    uint8_t event_type;
    uint8_t addr_type;
    uint8_t addr[6];
    const uint8_t* data; // 指向 AD payload
    uint8_t data_len;
    int8_t rssi;
};

// 解析 LE Connection Complete 事件（evt 指向 LE Meta 事件参数区，即 subevent 之后）。
static inline bool parse_le_connection_complete(const uint8_t* p, size_t len, LeConnectionComplete& out) {
    if (!p || len < 18)
        return false;
    out.status = p[0];
    out.connection_handle = static_cast<uint16_t>(p[1] | (p[2] << 8));
    out.role = p[3];
    out.peer_addr_type = p[4];
    memcpy(out.peer_addr, &p[5], 6);
    out.interval = static_cast<uint16_t>(p[11] | (p[12] << 8));
    out.latency = static_cast<uint16_t>(p[13] | (p[14] << 8));
    out.supervision_to = static_cast<uint16_t>(p[15] | (p[16] << 8));
    return true;
}

// 解析 Disconnection Complete 事件（evt 指向 Event Code 之后的参数区）。
static inline bool parse_disconnection_complete(const uint8_t* p, size_t len, DisconnectionComplete& out) {
    if (!p || len < 4)
        return false;
    out.status = p[0];
    out.connection_handle = static_cast<uint16_t>(p[1] | (p[2] << 8));
    out.reason = p[3];
    return true;
}

// 解析单条 LE Advertising Report 条目，返回消费的字节数；失败返回 0。
// p 指向条目起始，len 为剩余字节数。
static inline size_t parse_le_advertising_report_entry(const uint8_t* p, size_t len, LeAdvertisingReportEntry& out) {
    if (!p || len < 10)
        return 0;
    out.event_type = p[0];
    out.addr_type = p[1];
    memcpy(out.addr, &p[2], 6);
    out.data_len = p[8];
    out.data = &p[9];
    if (len < static_cast<size_t>(9 + out.data_len + 1))
        return 0; // 数据不完整（还差 RSSI 字节）
    out.rssi = static_cast<int8_t>(p[9 + out.data_len]);
    return static_cast<size_t>(10 + out.data_len);
}

} // namespace hci
} // namespace ble
} // namespace auroraos

#endif // AURORA_HCI_PACKET_HPP
