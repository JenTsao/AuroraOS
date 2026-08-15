#ifndef AURORA_HAL_SECURE_STORAGE_HAL_HPP
#define AURORA_HAL_SECURE_STORAGE_HAL_HPP

#include <stdint.h>

namespace auroraos {
namespace hal {

// ========================================================
// Secure Storage HAL — per-device secret provisioning
// ========================================================
//
// Abstracts the source of per-device secrets (Secure Element, eFuse, or
// encrypted OTP). All implementations MUST fail closed: if a secret is not
// provisioned at manufacturing time, or cannot be read from hardware, they
// return false and the caller MUST refuse service rather than falling back
// to any default / all-zero / shared key.
class ISecureStorageHal {
public:
    virtual ~ISecureStorageHal() = default;

    // True iff a per-device key has been provisioned at manufacturing time.
    virtual bool is_provisioned() = 0;

    // Read the 32-byte SoftBus pre-shared key (HMAC-SHA256) and its
    // monotonically increasing version. Returns false (fail closed) if the
    // key is not provisioned or the underlying hardware read fails.
    virtual bool read_softbus_key(uint8_t key[32], uint32_t* version) = 0;
};

// Board-provided factory. The default is a weak symbol returning nullptr
// (fail closed); real hardware boards override it with a strong definition
// backed by a Secure Element / eFuse / encrypted OTP region.
ISecureStorageHal* get_secure_storage_hal();

} // namespace hal
} // namespace auroraos

#endif // AURORA_HAL_SECURE_STORAGE_HAL_HPP
