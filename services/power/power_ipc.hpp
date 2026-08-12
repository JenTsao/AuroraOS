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
    GetPowerState = 4
};

struct PowerRequest {
    PowerOpcode opcode;
};

struct PowerReply {
    int status; // 0 for success
    union {
        uint8_t battery_percent;
        PowerState current_state;
    } data;
};

} // namespace power_service
} // namespace auroraos

#endif // AURORAOS_POWER_IPC_HPP
