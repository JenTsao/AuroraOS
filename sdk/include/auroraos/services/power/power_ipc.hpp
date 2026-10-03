#ifndef AURORAOS_POWER_IPC_HPP
#define AURORAOS_POWER_IPC_HPP

#include <stdint.h>
#include "power_state.hpp"

namespace auroraos {
namespace power_service {

enum class PowerOpcode : uint32_t {
    AcquireWakeLock = 1,
    ReleaseWakeLock = 2,
    GetBatteryLevel = 3,
    GetPowerState = 4,
    ReleaseAllWakeLocks = 5,
    GetBatteryInfo = 6,
    SetProfile = 7,
    GetProfile = 8,
    GetWakeLockCount = 9,
    CanSleep = 10
};

// 唤醒锁申请参数（仅 AcquireWakeLock 使用）
struct WakeLockArgs {
    uint8_t kind;        // WakeLockKind: 0 = PARTIAL, 1 = SCREEN
    uint32_t timeout_ms; // 0xFFFFFFFF = 不超时（不推荐）
};

struct PowerRequest {
    PowerOpcode opcode;
    WakeLockArgs lock; // AcquireWakeLock
    uint32_t profile;  // SetProfile: PowerProfile 原始值
};

struct PowerReply {
    int status; // 0 for success

    union {
        uint8_t battery_percent;
        PowerState current_state;
        uint32_t profile;
        int32_t count;
        uint8_t flag;
        BatteryInfo battery;
    } data;
};

} // namespace power_service
} // namespace auroraos

#endif // AURORAOS_POWER_IPC_HPP
