#ifndef AURORA_HAL_BLE_HPP
#define AURORA_HAL_BLE_HPP

#include <stdint.h>
#include <stddef.h>

namespace auroraos {
namespace ble {

// ========================================================
// 硬件抽象层 (HAL): 蓝牙低功耗射频接口
// 
// 作用：解耦具体厂商的 SDK（如 Ambiq Cordio、Nordic SoftDevice），
// 统一 AuroraOS 的下行控制指令。
// ========================================================
namespace HalBle {

    // 初始化硬件射频芯片/协议栈
    void init();

    // 开始广播设备 (Advertising)
    void start_advertising(const char* device_name);

    // 停止广播
    void stop_advertising();

    // 断开当前连接
    void disconnect();

    // 推送特征值变更 (Notify / Indicate)
    // svc_uuid: 服务 UUID (如 0x180D)
    // data, len: 变更的数据
    void notify_characteristic(uint16_t svc_uuid, const uint8_t* data, size_t len);

} // namespace HalBle
} // namespace ble
} // namespace auroraos

#endif // AURORA_HAL_BLE_HPP
