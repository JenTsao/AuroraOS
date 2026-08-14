#ifndef AURORA_BLE_STACK_HPP
#define AURORA_BLE_STACK_HPP

#include <stdint.h>
#include <string.h>
#include "../../kernel/task/task.hpp"
#include "../../kernel/core/msg_queue.hpp"
#include "posix.hpp"
#include "../../net/ble/hal_ble.hpp"
#include "../../apps/notification_center.hpp"
#include "../../net/ble/ble_signature.hpp"

// ========================================================
// 蓝牙连接状态机
// ========================================================
enum class BleConnectionState : uint8_t {
    DISCONNECTED, // 断开连接，仅维持极低频的广播 (Advertising)
    ADVERTISING,  // 高频广播中 (等待手机连接)
    CONNECTING,   // 正在建立 LL 层连接
    CONNECTED     // 链路已建立，GATT 数据通道开启
};

// ========================================================
// 标准 SIG GATT 服务 UUID 定义 (16-bit)
// ========================================================
constexpr uint16_t GATT_SVC_DEVICE_INFO = 0x180A; // 设备信息服务
constexpr uint16_t GATT_SVC_HEART_RATE = 0x180D;  // 心率服务
constexpr uint16_t GATT_SVC_BATTERY = 0x180F;     // 电池服务

// 极客专属：AuroraOS 自定义 OTA 与 Lua 脚本传输服务 (128-bit UUID)
constexpr uint16_t GATT_SVC_AURORA_CUSTOM = 0xFF01;

// ========================================================
// 蓝牙底层硬件事件定义 (用于解耦芯片厂 SDK 的 HCI 层)
// ========================================================
struct BleHciEvent {
    uint8_t event_type;
    uint16_t connection_handle;
    uint8_t payload[16];
};

class BleManager {
private:
    BleConnectionState current_state_;

    // 异步消息队列：用于接收来自底层硬件中断的 HCI 事件，防止阻塞射频中断
    MessageQueue<BleHciEvent, 16> hci_event_queue_;

    // 缓存最新的设备特征值，当手机发起 Read 请求时直接返回
    uint8_t cached_battery_level_;
    uint8_t cached_heart_rate_;

    BleManager();

    // 内部注册所有的 GATT Services 和 Characteristics
    void build_gatt_profile();

public:
    static BleManager& instance();

    void init();

    BleConnectionState get_state() const;

    // ========================================================
    // 上层应用调用：推送最新状态 (解耦调用，零阻塞)
    // ========================================================
    void update_heart_rate(uint8_t bpm);
    void update_battery_level(uint8_t level);

    // ========================================================
    // 底层硬件中断钩子 (ISR)：严禁执行任何阻塞操作！
    // ========================================================
    void on_hci_hardware_event_isr(uint8_t event_type, uint16_t handle);

    // ========================================================
    // BLE 守护线程执行中枢 (运行在 HIGH 优先级)
    // ========================================================
    void daemon_task();
};

#endif // AURORA_BLE_STACK_HPP
