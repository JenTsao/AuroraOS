#include "cspace.hpp"
#include "task.hpp"

namespace auroraos {
namespace kernel {

bool CSpace::is_valid_slot(uint32_t slot_id) {
    return slot_id < static_cast<uint32_t>(MAX_CSPACE_SLOTS);
}

bool CSpace::is_slot_occupied(TaskControlBlock* task, uint32_t slot_id) {
    if (!task || !is_valid_slot(slot_id))
        return false;
    return (task->security.occupied_mask & (1u << slot_id)) != 0;
}

uint16_t CSpace::get_occupied_mask(TaskControlBlock* task) {
    return task ? task->security.occupied_mask : 0;
}

int CSpace::cap_alloc_slot(TaskControlBlock* task) {
    if (!task)
        return -1;

    // 硬件位图一次性寻找最低空闲槽位 (Single CTZ instruction)
    uint32_t free_mask = (~static_cast<uint32_t>(task->security.occupied_mask)) & ((1u << MAX_CSPACE_SLOTS) - 1u);
    if (free_mask == 0)
        return -1; // CSpace 已满

    return Arch::find_lowest_bit(free_mask);
}

bool CSpace::cap_insert(TaskControlBlock* task, uint32_t slot_id, const Capability& cap) {
    if (!task || !is_valid_slot(slot_id) || cap.type == CapType::Null)
        return false;

    cap_delete(task, slot_id);

    task->security.cspace[slot_id] = cap;
    if (cap.object) {
        cap.object->retain();
    }
    task->security.occupied_mask |= (1u << slot_id);
    return true;
}

Capability* CSpace::cap_lookup(TaskControlBlock* task, uint32_t slot_id) {
    if (!task || !is_valid_slot(slot_id))
        return nullptr;

    Capability* cap = &task->security.cspace[slot_id];
    if (cap->type == CapType::Null) {
        task->security.occupied_mask &= ~(1u << slot_id);
        return nullptr;
    }

    // 确保位图状态与实际槽位一致
    task->security.occupied_mask |= (1u << slot_id);
    return cap;
}

bool CSpace::cap_delete(TaskControlBlock* task, uint32_t slot_id) {
    if (!task || !is_valid_slot(slot_id))
        return false;

    Capability& cap = task->security.cspace[slot_id];
    if (cap.type != CapType::Null && cap.object) {
        if (cap.type == CapType::Endpoint) {
            Endpoint* ep = static_cast<Endpoint*>(cap.object);
            if (task->ipc.waiting_endpoint == ep) {
                ep->cancel_waiter(task, IpcStatus::NoPermission);
            }
        }
        cap.object->release();
    }
    cap.type = CapType::Null;
    cap.object = nullptr;
    task->security.occupied_mask &= ~(1u << slot_id);
    return true;
}

bool CSpace::cap_derive_internal(TaskControlBlock* task, uint32_t src_slot, uint32_t dst_slot, uint32_t rights,
                                 uint32_t badge) {
    if (!task)
        return false;

    if (!is_valid_slot(src_slot) || !is_valid_slot(dst_slot))
        return false;

    const Capability& src_cap = task->security.cspace[src_slot];
    if (src_cap.type == CapType::Null)
        return false;

    bool req_r = rights & CAP_RIGHT_READ;
    bool req_w = rights & CAP_RIGHT_WRITE;
    bool req_g = rights & CAP_RIGHT_GRANT;

    // Privilege escalation check: requested rights must be a subset of source rights
    if ((req_r && !src_cap.rights.read) || (req_w && !src_cap.rights.write) || (req_g && !src_cap.rights.grant)) {
        return false;
    }

    // Before overwriting dst_cap, release if it holds something
    cap_delete(task, dst_slot);

    Capability& dst_cap = task->security.cspace[dst_slot];
    dst_cap.type = src_cap.type;
    dst_cap.object = src_cap.object;
    if (dst_cap.object) {
        dst_cap.object->retain();
    }
    dst_cap.rights.read = req_r;
    dst_cap.rights.write = req_w;
    dst_cap.rights.grant = req_g;
    dst_cap.badge = badge; // derive: inherit, mint: new value
    task->security.occupied_mask |= (1u << dst_slot);
    return true;
}

bool CSpace::cap_derive(TaskControlBlock* task, uint32_t src_slot, uint32_t dst_slot, uint32_t rights) {
    if (!task || !is_valid_slot(src_slot))
        return false;

    return cap_derive_internal(task, src_slot, dst_slot, rights, task->security.cspace[src_slot].badge);
}

bool CSpace::cap_mint(TaskControlBlock* task, uint32_t src_slot, uint32_t dst_slot, uint32_t rights, uint32_t badge) {
    return cap_derive_internal(task, src_slot, dst_slot, rights, badge);
}

bool CSpace::cap_revoke(TaskControlBlock* task, uint32_t slot_id) {
    if (!task || !is_valid_slot(slot_id))
        return false;

    Capability* src_cap = cap_lookup(task, slot_id);
    if (!src_cap || src_cap->object == nullptr)
        return false;

    KernelObject* target_obj = src_cap->object;

    // Scan all tasks and nullify capabilities pointing to the same object.
    // Skip the source slot itself so the owner retains access.
    int total_tasks = Scheduler::instance().get_task_count();
    for (int i = 0; i < total_tasks; i++) {
        TaskControlBlock* t = Scheduler::instance().get_task(i);
        if (!t)
            continue;

        // 同步位图状态
        for (int j = 0; j < MAX_CSPACE_SLOTS; ++j) {
            if (t->security.cspace[j].type != CapType::Null) {
                t->security.occupied_mask |= (1u << j);
            } else {
                t->security.occupied_mask &= ~(1u << j);
            }
        }

        uint32_t mask = t->security.occupied_mask;
        if (t == task) {
            mask &= ~(1u << slot_id); // 保留源 slot 自己
        }
        if (mask == 0)
            continue;

        // 仅循环已占用的 slot（利用硬件 CTZ 指令加速扫描）
        while (mask != 0) {
            int j = Arch::find_lowest_bit(mask);
            mask &= ~(1u << j);

            if (t->security.cspace[j].object == target_obj) {
                if (t->security.cspace[j].type == CapType::Endpoint && target_obj) {
                    Endpoint* ep = static_cast<Endpoint*>(target_obj);
                    if (t->ipc.waiting_endpoint == ep) {
                        ep->cancel_waiter(t, IpcStatus::NoPermission);
                    }
                }
                if (t->security.cspace[j].object) {
                    t->security.cspace[j].object->release();
                }
                t->security.cspace[j].type = CapType::Null;
                t->security.cspace[j].object = nullptr;
                t->security.occupied_mask &= ~(1u << j);
            }
        }
    }
    return true;
}

bool CSpace::cap_grant(TaskControlBlock* src_task, TaskControlBlock* dst_task, uint32_t src_slot, uint32_t dst_slot,
                       uint32_t new_rights, uint32_t badge) {
    if (!src_task || !dst_task)
        return false;

    if (!is_valid_slot(src_slot) || !is_valid_slot(dst_slot))
        return false;

    const Capability& src_cap = src_task->security.cspace[src_slot];
    if (src_cap.type == CapType::Null)
        return false;

    bool req_r = new_rights & CAP_RIGHT_READ;
    bool req_w = new_rights & CAP_RIGHT_WRITE;
    bool req_g = new_rights & CAP_RIGHT_GRANT;

    // Privilege escalation check
    if ((req_r && !src_cap.rights.read) || (req_w && !src_cap.rights.write) || (req_g && !src_cap.rights.grant)) {
        return false;
    }

    cap_delete(dst_task, dst_slot);

    Capability& dst_cap = dst_task->security.cspace[dst_slot];
    dst_cap.type = src_cap.type;
    dst_cap.object = src_cap.object;
    if (dst_cap.object) {
        dst_cap.object->retain();
    }
    dst_cap.rights.read = req_r;
    dst_cap.rights.write = req_w;
    dst_cap.rights.grant = req_g;
    dst_cap.badge = badge;
    dst_task->security.occupied_mask |= (1u << dst_slot);
    return true;
}

} // namespace kernel
} // namespace auroraos
