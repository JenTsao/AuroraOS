#ifndef KERNEL_FIRMWARE_HEADER_HPP
#define KERNEL_FIRMWARE_HEADER_HPP

#include <stdint.h>

namespace aurora {

constexpr uint32_t FIRMWARE_MAGIC = 0x41555241; // 'AURA'

enum class FirmwareStatus : uint32_t {
    EMPTY          = 0xFFFFFFFF,
    UPDATE_PENDING = 0x55AA55AA,
    VALID          = 0x12345678,
    CORRUPT        = 0xDEADBEEF
};

// Now 128 bytes total
struct FirmwareHeader {
    uint32_t magic;              // 4 bytes
    uint32_t version;            // 4 bytes
    uint32_t image_size;         // 4 bytes
    FirmwareStatus status;       // 4 bytes
    uint8_t  signature[64];      // 64 bytes (Ed25519)
    uint32_t reserved[12];       // 48 bytes -> 4 + 4 + 4 + 4 + 64 + 48 = 128 bytes
};

} // namespace aurora

#endif // KERNEL_FIRMWARE_HEADER_HPP
