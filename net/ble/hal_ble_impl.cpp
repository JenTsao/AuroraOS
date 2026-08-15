#include "hal_ble.hpp"
#include "hal_ble_impl.hpp"
#include "hci/hci_packet.hpp"
#include "hci/hci_transport.hpp"

#include <string.h>

// ========================================================
// HalBle 真实硬件实现 (Host -> Controller HCI Command)
//
// 将 AuroraOS 的 BLE 下行控制抽象映射为真实的 HCI Command，
// 通过全局 g_hci_transport (UART H4 / 内存 IPC) 下发给底层
// BLE Controller。所有状态均为文件内静态，零动态分配。
//
// 注意：本文件仅在固件 (firmware) 构建中编译。host 单元测试
// 使用 tests/stubs/kernel_stubs.cpp 中的空 stub，因此二者不会
// 产生符号冲突。
// ========================================================
namespace auroraos {
namespace ble {

// 引入 hci 命名空间的常用符号，避免冗长前缀。
using hci::g_hci_transport;
using hci::HCI_CMD_MAX_LEN;
using hci::BLE_ADV_DATA_MAX;

namespace {

// ---- 内部连接/广播状态 ----
volatile uint16_t g_conn_handle = 0xFFFF; // 0xFFFF = 无连接
volatile bool g_advertising = false;

// ---- 连接句柄 → 对端 MAC 映射 (固定容量) ----
struct ConnMacEntry {
    uint16_t handle;
    uint8_t mac[6];
    bool used;
};
ConnMacEntry g_conn_mac[8];

// ---- GATT service UUID → ATT handle 映射 (固定容量) ----
struct AttHandleEntry {
    uint16_t svc_uuid;
    uint16_t char_handle;
    bool used;
};
AttHandleEntry g_att_map[8];

// 当前广播的 AD 数据缓存（供重连/重新广播时复用）
uint8_t g_adv_data[BLE_ADV_DATA_MAX];
uint8_t g_adv_data_len = 0;

// 向 Controller 发送一条 HCI Command，失败静默（HAL 无阻塞，不抛异常）。
void send_hci_cmd(const uint8_t* cmd, size_t len) {
    if (g_hci_transport == nullptr || cmd == nullptr || len == 0)
        return;
    g_hci_transport->send_cmd(cmd, len);
}

} // namespace

// ========================================================
// 内部状态接口实现
// ========================================================
void hal_ble_set_conn_handle(uint16_t handle) {
    g_conn_handle = handle;
}

uint16_t hal_ble_get_conn_handle() {
    return g_conn_handle;
}

bool hal_ble_is_advertising() {
    return g_advertising;
}

void hal_ble_remember_conn_mac(uint16_t handle, const uint8_t mac[6]) {
    for (int i = 0; i < 8; ++i) {
        if (g_conn_mac[i].used && g_conn_mac[i].handle == handle) {
            memcpy(g_conn_mac[i].mac, mac, 6);
            return;
        }
    }
    for (int i = 0; i < 8; ++i) {
        if (!g_conn_mac[i].used) {
            g_conn_mac[i].used = true;
            g_conn_mac[i].handle = handle;
            memcpy(g_conn_mac[i].mac, mac, 6);
            return;
        }
    }
}

bool hal_ble_query_conn_mac(uint16_t handle, uint8_t mac_out[6]) {
    for (int i = 0; i < 8; ++i) {
        if (g_conn_mac[i].used && g_conn_mac[i].handle == handle) {
            memcpy(mac_out, g_conn_mac[i].mac, 6);
            return true;
        }
    }
    return false;
}

bool hal_ble_take_conn_mac(uint16_t handle, uint8_t mac_out[6]) {
    for (int i = 0; i < 8; ++i) {
        if (g_conn_mac[i].used && g_conn_mac[i].handle == handle) {
            memcpy(mac_out, g_conn_mac[i].mac, 6);
            g_conn_mac[i].used = false;
            return true;
        }
    }
    return false;
}

bool hal_ble_register_att_handle(uint16_t svc_uuid, uint16_t char_handle) {
    for (int i = 0; i < 8; ++i) {
        if (!g_att_map[i].used) {
            g_att_map[i].svc_uuid = svc_uuid;
            g_att_map[i].char_handle = char_handle;
            g_att_map[i].used = true;
            return true;
        }
    }
    return false;
}

uint16_t hal_ble_lookup_att_handle(uint16_t svc_uuid) {
    for (int i = 0; i < 8; ++i) {
        if (g_att_map[i].used && g_att_map[i].svc_uuid == svc_uuid)
            return g_att_map[i].char_handle;
    }
    return 0; // 0 = 无效 ATT handle
}

// ========================================================
// HalBle 公开接口实现
// ========================================================
namespace HalBle {

void init() {
    // 复位 Controller，进入已知状态。
    uint8_t cmd[HCI_CMD_MAX_LEN];
    size_t len = hci::build_reset_cmd(cmd);
    send_hci_cmd(cmd, len);
    g_conn_handle = 0xFFFF;
    g_advertising = false;
}

void start_advertising(const char* device_name) {
    // 标准广播：构建 Flags + Complete Local Name 的 AD payload。
    uint8_t ad[BLE_ADV_DATA_MAX];
    size_t pos = 0;
    // AD Structure 1: Flags — LE General Discoverable + BR/EDR Not Supported
    ad[pos++] = 0x02;
    ad[pos++] = 0x01; // Flags
    ad[pos++] = 0x02 | 0x04;
    // AD Structure 2: Complete Local Name
    // Bound the copy by the remaining AD space (reserving length + type bytes),
    // then by the NUL terminator — check the bound first so we never read past
    // the caller's buffer when it is exactly full.
    size_t name_len = 0;
    if (device_name) {
        const size_t max_name = BLE_ADV_DATA_MAX - pos - 2;
        while (name_len < max_name && device_name[name_len])
            ++name_len;
    }
    if (name_len > 0) {
        ad[pos++] = static_cast<uint8_t>(name_len + 1);
        ad[pos++] = 0x09; // Complete Local Name
        memcpy(&ad[pos], device_name, name_len);
        pos += name_len;
    }

    // 缓存 AD 数据
    memcpy(g_adv_data, ad, pos);
    g_adv_data_len = static_cast<uint8_t>(pos);

    // 1) 设置广播参数 (可连接、非定向)
    uint8_t cmd[HCI_CMD_MAX_LEN];
    send_hci_cmd(cmd, hci::build_le_set_advertising_parameters(cmd, true));
    // 2) 设置广播数据
    send_hci_cmd(cmd, hci::build_le_set_advertising_data(cmd, g_adv_data, g_adv_data_len));
    // 3) 使能广播
    send_hci_cmd(cmd, hci::build_le_set_advertise_enable(cmd, true));
    g_advertising = true;
}

void start_advertising_raw(const uint8_t* ad_data, size_t ad_len) {
    if (ad_len > BLE_ADV_DATA_MAX)
        ad_len = BLE_ADV_DATA_MAX;
    memcpy(g_adv_data, ad_data ? ad_data : g_adv_data, ad_len);
    g_adv_data_len = static_cast<uint8_t>(ad_len);

    uint8_t cmd[HCI_CMD_MAX_LEN];
    send_hci_cmd(cmd, hci::build_le_set_advertising_parameters(cmd, true));
    send_hci_cmd(cmd, hci::build_le_set_advertising_data(cmd, g_adv_data, g_adv_data_len));
    send_hci_cmd(cmd, hci::build_le_set_advertise_enable(cmd, true));
    g_advertising = true;
}

void stop_advertising() {
    uint8_t cmd[HCI_CMD_MAX_LEN];
    send_hci_cmd(cmd, hci::build_le_set_advertise_enable(cmd, false));
    g_advertising = false;
}

void disconnect() {
    uint16_t handle = g_conn_handle;
    if (handle == 0xFFFF)
        return;
    uint8_t cmd[HCI_CMD_MAX_LEN];
    // 0x13 = Remote User Terminated Connection
    send_hci_cmd(cmd, hci::build_disconnect_cmd(cmd, handle, 0x13));
    // 连接句柄由 Disconnection Complete 事件异步清零，此处不立即置 0xFFFF。
}

void notify_characteristic(uint16_t svc_uuid, const uint8_t* data, size_t len) {
    uint16_t conn = g_conn_handle;
    if (conn == 0xFFFF || data == nullptr || len == 0)
        return;

    uint16_t att_handle = hal_ble_lookup_att_handle(svc_uuid);
    if (att_handle == 0)
        return; // 该 service 未注册可 Notify 的特征值

    // 构建 ATT Handle Value Notification (opcode 0x1B)
    // PDU: [0x1B, handle_lo, handle_hi, value...]
    uint8_t pdu[4 + 255];
    pdu[0] = 0x1B;
    pdu[1] = static_cast<uint8_t>(att_handle & 0xFF);
    pdu[2] = static_cast<uint8_t>(att_handle >> 8);
    if (len > 255)
        len = 255;
    memcpy(&pdu[3], data, len);
    size_t pdu_len = 3 + len;

    // 构建 ACL Data 头 (handle 12bit | PB 2bit | BC 2bit + 2 字节长度)
    uint8_t acl[4 + 259];
    uint16_t handle_pb = static_cast<uint16_t>((conn & 0x0FFF) | (0x00 << 12) | (0x00 << 14));
    acl[0] = static_cast<uint8_t>(handle_pb & 0xFF);
    acl[1] = static_cast<uint8_t>(handle_pb >> 8);
    acl[2] = static_cast<uint8_t>(pdu_len & 0xFF);
    acl[3] = static_cast<uint8_t>(pdu_len >> 8);
    memcpy(&acl[4], pdu, pdu_len);

    if (g_hci_transport != nullptr)
        g_hci_transport->send_acl(acl, 4 + pdu_len);
}

} // namespace HalBle
} // namespace ble
} // namespace auroraos
