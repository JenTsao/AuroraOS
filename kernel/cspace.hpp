#ifndef CSPACE_HPP
#define CSPACE_HPP

#include <stdint.h>

namespace auroraos {
namespace kernel {

enum class CapType : uint8_t {
    Null = 0,
    Endpoint = 1,
    Thread = 2,
    Memory = 3
};

struct CapRights {
    bool read    : 1;
    bool write   : 1;
    bool grant   : 1;
    uint8_t rsvd : 5;
};

struct Capability {
    CapType type;
    CapRights rights;
    uint32_t badge; // Endpoint badge for IPC identification
    void* object; // Pointer to Endpoint, TCB, or Memory
};

constexpr int MAX_CSPACE_SLOTS = 16;

} // namespace kernel
} // namespace auroraos

#endif // CSPACE_HPP
