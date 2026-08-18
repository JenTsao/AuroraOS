// =============================================================================
// kernel/core/syscall_ipc.hpp
//
// IPC 系统调用内核态分发与处理实现 (KernelIpc)
// 包含权限校验、超时与非阻塞支持、以及内核调度协同
// =============================================================================
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
    // 用户态 sys_ipc_call 系统调用处理接口 (支持超时与非阻塞)
    static int sys_ipc_call(TaskControlBlock* task, uint32_t cap_slot, void* msg, uint32_t len,
                            void* reply_buf, uint32_t max_reply_len,
                            uint32_t timeout_ticks = IPC_TIMEOUT_INFINITE) {
        Capability* cap = CSpace::cap_lookup(task, cap_slot);
        if (!cap || cap->type != CapType::Endpoint || !cap->rights.write) {
            return static_cast<int>(IpcStatus::NoPermission);
        }
        Endpoint* ep = static_cast<Endpoint*>(cap->object);
        if (!ep)
            return static_cast<int>(IpcStatus::Invalid);

        IpcStatus st = ep->call(task, msg, len, reply_buf, max_reply_len, timeout_ticks);

        if (st == IpcStatus::WouldBlock) {
            return static_cast<int>(IpcStatus::WouldBlock);
        }

        if (len >= sizeof(auroraos::kernel::IpcRawMessage) && msg) {
            const auto* hdr = static_cast<const auroraos::kernel::IpcRawMessage*>(msg);
            task->ipc.msg_type = static_cast<uint32_t>(hdr->msg_type);
        }

        if (task->ipc.state == auroraos::kernel::IpcState::Sending ||
            task->ipc.state == auroraos::kernel::IpcState::ReplyBlocked) {
            Scheduler::instance().schedule(); // 阻塞并触发调度
        }
        return static_cast<int>(task->ipc.status);
    }

    // 用户态 sys_ipc_receive 系统调用处理接口 (支持超时与非阻塞)
    static int sys_ipc_receive(TaskControlBlock* task, uint32_t cap_slot, void* msg_buf, uint32_t max_len,
                              uint32_t* out_sender_id = nullptr,
                              uint32_t timeout_ticks = IPC_TIMEOUT_INFINITE) {
        Capability* cap = CSpace::cap_lookup(task, cap_slot);
        if (!cap || cap->type != CapType::Endpoint || !cap->rights.read) {
            return static_cast<int>(IpcStatus::NoPermission);
        }
        Endpoint* ep = static_cast<Endpoint*>(cap->object);
        if (!ep)
            return static_cast<int>(IpcStatus::Invalid);

        IpcStatus st = ep->receive(task, msg_buf, max_len, timeout_ticks);
        if (st == IpcStatus::WouldBlock) {
            return static_cast<int>(IpcStatus::WouldBlock);
        }

        if (task->ipc.msg_len >= sizeof(auroraos::kernel::IpcRawMessage) && task->ipc.msg_buf) {
            const auto* hdr = static_cast<const auroraos::kernel::IpcRawMessage*>(task->ipc.msg_buf);
            task->ipc.msg_type = static_cast<uint32_t>(hdr->msg_type);
        }

        if (task->ipc.state == auroraos::kernel::IpcState::Receiving) {
            Scheduler::instance().schedule(); // 阻塞并等待发送方唤醒
        } else if (out_sender_id) {
            *out_sender_id = task->ipc.sender_id;
        }
        return static_cast<int>(task->ipc.status);
    }

    // 用户态 sys_ipc_reply 系统调用处理接口
    static int sys_ipc_reply(TaskControlBlock* task, uint32_t sender_id, void* reply_msg, uint32_t len) {
        IpcStatus st = Endpoint::reply(task, sender_id, reply_msg, len);
        Scheduler::instance().schedule();
        return static_cast<int>(st);
    }
};

} // namespace kernel
} // namespace auroraos

#endif // SYSCALL_IPC_HPP
