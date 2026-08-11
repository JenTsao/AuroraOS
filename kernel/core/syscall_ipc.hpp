#ifndef SYSCALL_IPC_HPP
#define SYSCALL_IPC_HPP

#include <stdint.h>
#include "ipc.hpp"
#include "cspace.hpp"
#include "../task/task.hpp"

namespace auroraos {
namespace kernel {

class KernelIpc {
public:
    // User syscall handlers in kernel space
    static bool sys_ipc_call(TaskControlBlock* task, uint32_t cap_slot, void* msg, uint32_t len, void* reply_buf, uint32_t max_reply_len) {
        Capability* cap = CSpace::cap_lookup(task, cap_slot);
        if (!cap || cap->type != CapType::Endpoint || !cap->rights.write) {
            return false; // Invalid capability or missing write rights
        }
        Endpoint* ep = static_cast<Endpoint*>(cap->object);
        if (!ep) return false;
        
        ep->call(task, msg, len, reply_buf, max_reply_len);
        return true;
    }

    static bool sys_ipc_receive(TaskControlBlock* task, uint32_t cap_slot, void* msg_buf, uint32_t max_len) {
        Capability* cap = CSpace::cap_lookup(task, cap_slot);
        if (!cap || cap->type != CapType::Endpoint || !cap->rights.read) {
            return false; // Invalid capability or missing read rights
        }
        Endpoint* ep = static_cast<Endpoint*>(cap->object);
        if (!ep) return false;
        
        ep->receive(task, msg_buf, max_len);
        return true;
    }

    static bool sys_ipc_reply(TaskControlBlock* task, uint32_t sender_id, void* reply_msg, uint32_t len) {
        // Reply doesn't require a capability slot; authority is derived from having received the message
        Endpoint::reply(task, sender_id, reply_msg, len);
        return true;
    }
};

} // namespace kernel
} // namespace auroraos

#endif // SYSCALL_IPC_HPP
