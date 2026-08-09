#include "cspace.hpp"
#include "task.hpp"

namespace auroraos {
namespace kernel {

bool CSpace::is_valid_slot(uint32_t slot_id) {
    return slot_id < static_cast<uint32_t>(MAX_CSPACE_SLOTS);
}

Capability* CSpace::cap_lookup(TaskControlBlock* task, uint32_t slot_id) {
    if (!task || !is_valid_slot(slot_id)) return nullptr;
    Capability* cap = &task->cspace[slot_id];
    if (cap->type == CapType::Null) return nullptr;
    return cap;
}

bool CSpace::cap_delete(TaskControlBlock* task, uint32_t slot_id) {
    if (!task || !is_valid_slot(slot_id)) return false;
    task->cspace[slot_id].type = CapType::Null;
    task->cspace[slot_id].object = nullptr;
    return true;
}

bool CSpace::cap_derive_internal(TaskControlBlock* task,
                                 uint32_t src_slot, uint32_t dst_slot,
                                 uint32_t rights, uint32_t badge) {
    if (!task) return false;
    if (!is_valid_slot(src_slot) || !is_valid_slot(dst_slot)) return false;

    const Capability& src_cap = task->cspace[src_slot];
    if (src_cap.type == CapType::Null) return false;

    bool req_r = rights & CAP_RIGHT_READ;
    bool req_w = rights & CAP_RIGHT_WRITE;
    bool req_g = rights & CAP_RIGHT_GRANT;

    // Privilege escalation check: requested rights must be a subset of source rights
    if ((req_r && !src_cap.rights.read) ||
        (req_w && !src_cap.rights.write) ||
        (req_g && !src_cap.rights.grant)) {
        return false;
    }

    Capability& dst_cap = task->cspace[dst_slot];
    dst_cap.type = src_cap.type;
    dst_cap.object = src_cap.object;
    dst_cap.rights.read = req_r;
    dst_cap.rights.write = req_w;
    dst_cap.rights.grant = req_g;
    dst_cap.badge = badge; // derive: inherit, mint: new value
    return true;
}

bool CSpace::cap_derive(TaskControlBlock* task,
                        uint32_t src_slot, uint32_t dst_slot,
                        uint32_t rights) {
    if (!task || !is_valid_slot(src_slot)) return false;
    return cap_derive_internal(task, src_slot, dst_slot, rights,
                               task->cspace[src_slot].badge);
}

bool CSpace::cap_mint(TaskControlBlock* task,
                      uint32_t src_slot, uint32_t dst_slot,
                      uint32_t rights, uint32_t badge) {
    return cap_derive_internal(task, src_slot, dst_slot, rights, badge);
}

bool CSpace::cap_revoke(TaskControlBlock* task, uint32_t slot_id) {
    if (!task || !is_valid_slot(slot_id)) return false;

    const Capability& src_cap = task->cspace[slot_id];
    if (src_cap.type == CapType::Null || src_cap.object == nullptr) return false;

    void* target_obj = src_cap.object;

    // Scan all tasks and nullify capabilities pointing to the same object.
    // Skip the source slot itself so the owner retains access.
    int total_tasks = Scheduler::instance().get_task_count();
    for (int i = 0; i < total_tasks; i++) {
        TaskControlBlock* t = Scheduler::instance().get_task(i);
        if (!t) continue;
        for (int j = 0; j < MAX_CSPACE_SLOTS; j++) {
            if (t == task && static_cast<uint32_t>(j) == slot_id) continue;
            if (t->cspace[j].object == target_obj) {
                t->cspace[j].type = CapType::Null;
                t->cspace[j].object = nullptr;
            }
        }
    }
    return true;
}

bool CSpace::cap_grant(TaskControlBlock* src_task, TaskControlBlock* dst_task,
                       uint32_t src_slot, uint32_t dst_slot,
                       uint32_t new_rights, uint32_t badge) {
    if (!src_task || !dst_task) return false;
    if (!is_valid_slot(src_slot) || !is_valid_slot(dst_slot)) return false;

    const Capability& src_cap = src_task->cspace[src_slot];
    if (src_cap.type == CapType::Null) return false;

    bool req_r = new_rights & CAP_RIGHT_READ;
    bool req_w = new_rights & CAP_RIGHT_WRITE;
    bool req_g = new_rights & CAP_RIGHT_GRANT;

    // Privilege escalation check
    if ((req_r && !src_cap.rights.read) ||
        (req_w && !src_cap.rights.write) ||
        (req_g && !src_cap.rights.grant)) {
        return false;
    }

    Capability& dst_cap = dst_task->cspace[dst_slot];
    dst_cap.type = src_cap.type;
    dst_cap.object = src_cap.object;
    dst_cap.rights.read = req_r;
    dst_cap.rights.write = req_w;
    dst_cap.rights.grant = req_g;
    dst_cap.badge = badge;
    return true;
}

} // namespace kernel
} // namespace auroraos
