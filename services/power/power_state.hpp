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

} // namespace power_service
} // namespace auroraos

#endif // AURORAOS_POWER_STATE_HPP
