#ifndef CSPACE_HPP
#define CSPACE_HPP

#include <stdint.h>

struct TaskControlBlock;

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

// Rights bitmask constants for syscall parameters
constexpr uint32_t CAP_RIGHT_READ  = 1;
constexpr uint32_t CAP_RIGHT_WRITE = 2;
constexpr uint32_t CAP_RIGHT_GRANT = 4;

class CSpace {
public:
    // Lookup a capability in a task's cspace.
    // Returns nullptr if slot_id is out of range or slot is Null.
    static Capability* cap_lookup(TaskControlBlock* task, uint32_t slot_id);

    // Delete (nullify) a capability slot.
    // Returns true on success, false if slot_id is out of range.
    static bool cap_delete(TaskControlBlock* task, uint32_t slot_id);

    // Derive: copy src->dst with rights downgrade only (cannot escalate).
    // Badge is inherited from source.
    // Returns true on success, false on escalation or invalid parameters.
    static bool cap_derive(TaskControlBlock* task,
                           uint32_t src_slot, uint32_t dst_slot,
                           uint32_t rights);

    // Mint: like derive but sets a new badge value.
    // Returns true on success, false on escalation or invalid parameters.
    static bool cap_mint(TaskControlBlock* task,
                         uint32_t src_slot, uint32_t dst_slot,
                         uint32_t rights, uint32_t badge);

    // Revoke: nullify all capabilities pointing to the same object
    // across ALL tasks. The source slot itself is preserved.
    // Returns true on success, false if source is null or out of range.
    static bool cap_revoke(TaskControlBlock* task, uint32_t slot_id);

    // Grant: cross-task capability transfer.
    // Source rights must cover requested new_rights (no escalation).
    // Returns true on success, false on escalation or invalid parameters.
    static bool cap_grant(TaskControlBlock* src_task, TaskControlBlock* dst_task,
                          uint32_t src_slot, uint32_t dst_slot,
                          uint32_t new_rights, uint32_t badge);

    // Validate cspace slot index.
    static bool is_valid_slot(uint32_t slot_id);

private:
    // Internal: derive/mint shared logic. If badge is non-zero, it's minted.
    static bool cap_derive_internal(TaskControlBlock* task,
                                    uint32_t src_slot, uint32_t dst_slot,
                                    uint32_t rights, uint32_t badge);
};

} // namespace kernel
} // namespace auroraos

#endif // CSPACE_HPP
