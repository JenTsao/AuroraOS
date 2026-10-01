#ifndef AURORAOS_POWER_STATE_HPP
#define AURORAOS_POWER_STATE_HPP

#include <stdint.h>

namespace auroraos {
namespace power_service {

enum class PowerState : uint8_t {
    RUN = 0,
    IDLE = 1,
    LIGHT_SLEEP = 2,
    DEEP_SLEEP = 3,
    SHUTDOWN = 4
};

// 唤醒锁类型：与内核 WakeLockRegistry 语义一致
enum class WakeLockKind : uint8_t {
    PARTIAL = 0, // 阻止深睡，允许息屏
    SCREEN = 1   // 额外要求屏幕保持点亮
};

// 电池遥测快照：一次 IPC 取全，避免客户端为电压/温度/充电态各发一次请求
struct BatteryInfo {
    uint8_t level;        // 0-100
    uint8_t health;       // BatteryHealth 原始值
    uint8_t charge_state; // ChargeState 原始值
    uint8_t plugged;      // 0/1
    uint16_t voltage_mv;
    int16_t temperature_c;
};

} // namespace power_service
} // namespace auroraos

#endif // AURORAOS_POWER_STATE_HPP
