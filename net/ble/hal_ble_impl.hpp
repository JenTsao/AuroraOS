#ifndef AURORA_HAL_BLE_IMPL_HPP
#define AURORA_HAL_BLE_IMPL_HPP

#include <stdint.h>

// ========================================================
// HalBle 真实硬件实现 — 内部状态接口
//
// 本头文件暴露 HalBle 实现的内部状态更新接口，供：
//   - HCI 事件分发器 (hci_event_dispatch.hpp) 在收到
//     LE Connection Complete / Disconnection Complete 时
//     更新连接句柄；
//   - BleManager::build_gatt_profile() 在注册 GATT 表时
//     填充 service UUID → ATT handle 映射。
//
// HalBle 的公开函数体 (init / start_advertising / ...) 定义于
// net/ble/hal_ble_impl.cpp，通过全局 HciTransport 下发真实 HCI 命令。
// ========================================================
namespace auroraos {
namespace ble {

// ---- 内部状态更新 (由 HCI 事件分发器调用) ----
// 更新当前连接句柄；0xFFFF 表示已断开。
void hal_ble_set_conn_handle(uint16_t handle);
uint16_t hal_ble_get_conn_handle();

// 记录广播状态 (由 HalBle 自身维护，供诊断查询)。
bool hal_ble_is_advertising();

// ---- 连接句柄 → 对端 MAC 映射 (由 HCI 事件分发器填充/查询) ----
// remember: 记录 handle 对应的对端 MAC（连接建立时调用）。
void hal_ble_remember_conn_mac(uint16_t handle, const uint8_t mac[6]);
// query: 非破坏性查询 handle 对应的 MAC。
bool hal_ble_query_conn_mac(uint16_t handle, uint8_t mac_out[6]);
// take: 查询并删除映射（断开时调用）。
bool hal_ble_take_conn_mac(uint16_t handle, uint8_t mac_out[6]);

// ---- GATT ATT Handle 映射 (由 BleManager::build_gatt_profile 填充) ----
// 将 16-bit 服务 UUID 映射到该服务特征值在本机 GATT 表中的 ATT handle，
// 供 notify_characteristic() 编码 ATT Handle Value Notification。
bool hal_ble_register_att_handle(uint16_t svc_uuid, uint16_t char_handle);
uint16_t hal_ble_lookup_att_handle(uint16_t svc_uuid);

} // namespace ble
} // namespace auroraos

#endif // AURORA_HAL_BLE_IMPL_HPP
