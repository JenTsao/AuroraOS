// hal_ble_stub.cpp — Host-side stub for auroraos::ble::HalBle hardware abstraction layer.
// All functions are no-ops; the real implementations live in vendor-specific BSP code.

#include "../../net/ble/hal_ble.hpp"

namespace auroraos {
namespace ble {
namespace HalBle {

    void init() {}

    void start_advertising(const char*) {}

    void start_advertising_raw(const uint8_t*, size_t) {}

    void stop_advertising() {}

    void disconnect() {}

    void notify_characteristic(uint16_t, const uint8_t*, size_t) {}

} // namespace HalBle
} // namespace ble
} // namespace auroraos
