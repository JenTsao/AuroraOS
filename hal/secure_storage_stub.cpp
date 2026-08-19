// ========================================================
// secure_storage_stub.cpp — weak default Secure Storage HAL
//
// Provides a fail-closed default for auroraos::hal::get_secure_storage_hal()
// that returns nullptr (no Secure Element / OTP provisioned). Real hardware
// boards override this weak symbol with a strong definition that reads the
// per-device key from a Secure Element, eFuse, or encrypted OTP region.
//
// Returning nullptr guarantees that DistributedSoftBus::init() leaves its
// key slots invalid, so verify_hmac() rejects every peer and
// broadcast_beacon() stays silent — the same fail-closed behaviour the
// previous #error enforced, but without blocking compilation of boards
// that genuinely have no Secure Element.
// ========================================================

#include "secure_storage_hal.hpp"

namespace auroraos {
namespace hal {

__attribute__((weak)) ISecureStorageHal* get_secure_storage_hal() {
    return nullptr;
}

} // namespace hal
} // namespace auroraos
