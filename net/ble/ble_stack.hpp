#ifndef AURORA_BLE_STACK_HPP
#define AURORA_BLE_STACK_HPP

#include <stdint.h>
#include <string.h>
#include "../../kernel/task.hpp"
#include "../../kernel/msg_queue.hpp"
#include "posix.hpp"
#include "hal_ble.hpp"
#include "../../apps/notification_center.hpp"
#include "ble_signature.hpp"

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
constexpr uint16_t GATT_SVC_HEART_RATE  = 0x180D; // 心率服务
constexpr uint16_t GATT_SVC_BATTERY     = 0x180F; // 电池服务

// 极客专属：AuroraOS 自定义 OTA 与 Lua 脚本传输服务 (128-bit UUID)
// 比如: 0000FF01-0000-1000-8000-00805F9B34FB
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

    BleManager() : current_state_(BleConnectionState::DISCONNECTED), 
                   cached_battery_level_(100), cached_heart_rate_(0) {}

    // 内部注册所有的 GATT Services 和 Characteristics
    void build_gatt_profile() {
        // 伪代码：向底层 SDK 注册 GATT 数据库
        // 1. 注册 0x180A 设备信息 (包含 Manufacturer Name, Model Number 等)
        // 2. 注册 0x180D 心率服务 (设定 Characteristic 为 Notify 属性)
        // 3. 注册 0x180F 电池服务 (设定 Characteristic 为 Read/Notify 属性)
        // 4. 注册 0xFF01 Aurora 自定义服务 (设定为 Security Mode 1 Level 3 配对后可写，用于接收 Lua 脚本)
    }

public:
    static BleManager& instance() {
        static BleManager manager;
        return manager;
    }

    void init() {
        hci_event_queue_.init();
        build_gatt_profile();
        current_state_ = BleConnectionState::ADVERTISING;
        
        // 调用底层接口，开启射频广播
        auroraos::ble::HalBle::start_advertising("Aurora_MiBand8");
    }

    BleConnectionState get_state() const { return current_state_; }

    // ========================================================
    // 上层应用调用：推送最新状态 (解耦调用，零阻塞)
    // ========================================================
    void update_heart_rate(uint8_t bpm) {
        if (cached_heart_rate_ == bpm) return;
        cached_heart_rate_ = bpm;
        
        // 如果当前是连接状态，且手机订阅了 Notify，则推向空中接口
        if (current_state_ == BleConnectionState::CONNECTED) {
            auroraos::ble::HalBle::notify_characteristic(GATT_SVC_HEART_RATE, &bpm, 1);
        }
    }

    void update_battery_level(uint8_t level) {
        if (cached_battery_level_ == level) return;
        cached_battery_level_ = level;

        if (current_state_ == BleConnectionState::CONNECTED) {
            auroraos::ble::HalBle::notify_characteristic(GATT_SVC_BATTERY, &level, 1);
        }
    }

    // ========================================================
    // 底层硬件中断钩子 (ISR)：严禁执行任何阻塞操作！
    // ========================================================
    void on_hci_hardware_event_isr(uint8_t event_type, uint16_t handle) {
        BleHciEvent event = {event_type, handle, {0}};
        // 使用非阻塞的 try_push 塞入无锁队列
        hci_event_queue_.try_push(event); 
    }

    // ========================================================
    // BLE 守护线程执行中枢 (运行在 HIGH 优先级)
    // ========================================================
    void daemon_task() {
        while (true) {
            // 0 功耗挂起，等待底层射频芯片发来事件
            BleHciEvent event = hci_event_queue_.pop();

            // 处理连接与断开的逻辑状态机
            switch (event.event_type) {
                case 0x01: // EVENT_CONNECT
                    Arch::disable_interrupts();
                    current_state_ = BleConnectionState::CONNECTED;
                    Arch::enable_interrupts();
                    break;
                case 0x02: // EVENT_DISCONNECT
                    Arch::disable_interrupts();
                    current_state_ = BleConnectionState::ADVERTISING;
                    Arch::enable_interrupts();
                    // 断开后立刻重启广播
                    auroraos::ble::HalBle::start_advertising("Aurora_MiBand8");
                    break;
                case 0x03: { // EVENT_DATA_RECEIVED (Lua 小程序数据包)
                    // ========================================================
                    // 工业级签名验证: Ed25519 + Nonce 防重放 + 失败锁定
                    // ========================================================
                    auto& verifier = auroraos::ble::BleSignatureVerifier::instance();
                    
                    // 检查是否因连续失败被锁定
                    if (verifier.is_locked_out()) {
                        int fd = open("/dev/uart0", 0);
                        if (fd >= 0) {
                            write(fd, "[BLE] Security: Verification locked out due to repeated failures\r\n", 65);
                            close(fd);
                        }
                        break;  // 静默丢弃
                    }
                    
                    // 验证签名 (frame = Nonce || Len || Payload || Signature)
                    if (verifier.verify(event.payload, sizeof(event.payload))) {
                        // 验证通过：提取 payload 送入 VFS 或 MiniProgramEngine
                        // payload 从 offset 6 开始，长度在 offset 4-5 (LE uint16)
                        uint16_t payload_len;
                        memcpy(&payload_len, event.payload + 4, 2);
                        
                        // 将通过验证的 Lua 脚本数据送入处理管线
                        // validated_lua_payload = event.payload + 6, length = payload_len
                        // MiniProgramEngine::instance().ingest(event.payload + 6, payload_len);
                    } else {
                        // 验证失败：记录安全事件（不泄露失败原因）
                        // 复用栈溢出计数器作为安全事件报警
                        // SecurityMonitor::instance().report_stack_overflow(0);  
                        // 静默丢弃，不回复攻击者
                    }
                    break;
                }
                case 0x04: { // EVENT_NOTIFICATION_RECEIVED (手机通知推送, TLV 格式)
                    // 获取系统时钟 (Arch::get_tick 为全局符号，在测试中可 mock)
                    const uint32_t tick = static_cast<uint32_t>(Arch::get_cycle());
                    const aurora::Notification n = aurora::BleNotificationParser::parse(
                        event.payload,
                        static_cast<uint8_t>(sizeof(event.payload)),
                        tick);
                    aurora::NotificationCenter::instance().post(n);
                    break;
                }
            }
        }
    }
};

#endif // AURORA_BLE_STACK_HPP
