#include "syscall_dispatcher.hpp"
#include "syscall_validator.hpp"
#include "syscall_ipc.hpp"
#include "../task/task.hpp"
#include "cspace.hpp"
#include "audit.hpp"
#include "../task/wait_queue.hpp"
#include "device.hpp"
#include "../../boot/interrupts.hpp" // For extern "C" uart_puts

extern "C" void uart_puts(const char* s);

namespace auroraos {
namespace kernel {

SyscallDispatcher::SyscallHandlerFn SyscallDispatcher::syscall_table[256];

void SyscallDispatcher::init() {
    for (int i = 0; i < 256; ++i) {
        syscall_table[i] = handle_unknown;
    }
    syscall_table[SYS_PRINT] = handle_print;
    syscall_table[SYS_YIELD] = handle_yield;
    syscall_table[SYS_SLEEP] = handle_sleep;
    syscall_table[SYS_CAP_DERIVE] = handle_cap_derive;
    syscall_table[SYS_CAP_MINT] = handle_cap_mint;
    syscall_table[SYS_CAP_REVOKE] = handle_cap_revoke;
    syscall_table[SYS_CAP_GRANT] = handle_cap_grant;
    syscall_table[SYS_CAP_DELETE] = handle_cap_delete;
    syscall_table[SYS_KILL] = handle_kill;
    syscall_table[SYS_SIGACTION] = handle_sigaction;
    syscall_table[SYS_SIGPROCMASK] = handle_sigprocmask;
    syscall_table[SYS_IPC_CALL] = handle_ipc_call;
    syscall_table[SYS_IPC_RECEIVE] = handle_ipc_receive;
    syscall_table[SYS_IPC_REPLY] = handle_ipc_reply;
    syscall_table[SYS_DEV_OPEN] = handle_dev_open;
    syscall_table[SYS_DEV_READ] = handle_dev_read;
    syscall_table[SYS_DEV_WRITE] = handle_dev_write;
    syscall_table[SYS_DEV_IOCTL] = handle_dev_ioctl;
    syscall_table[SYS_DEV_REGISTER] = handle_dev_register;
}

void SyscallDispatcher::dispatch(InterruptFrame* frame, uint8_t svc_number) {
    if (syscall_table[svc_number]) {
        syscall_table[svc_number](frame);
    } else {
        handle_unknown(frame);
    }
}

void SyscallDispatcher::handle_print(InterruptFrame* frame) {
    AUDIT_HOOK_SVC(SYS_PRINT, 0);
    TaskControlBlock* cur = Scheduler::instance().get_current_tcb();
    const char* str = reinterpret_cast<const char*>(frame->arg0);
    if (!str)
        return;
    constexpr size_t MAX_PRINT_LEN = 256u;
    if (!cur) {
        for (size_t i = 0; i < MAX_PRINT_LEN; i++) {
            if (str[i] == '\0') {
                uart_puts(str);
                return;
            }
        }
        return;
    }
    bool safe = false;
    for (size_t i = 0; i < MAX_PRINT_LEN; i++) {
        const bool valid = SyscallValidator::validate_user_ptr(str + i, 1, cur, false);
        if (!valid)
            break;
        if (str[i] == '\0') {
            safe = true;
            break;
        }
    }
    if (!safe) {
        uart_puts("[Kernel] SYS_PRINT: invalid ptr or no null terminator rejected\n");
        return;
    }
    uart_puts(str);
}

void SyscallDispatcher::handle_yield(InterruptFrame* /*frame*/) {
    AUDIT_HOOK_SVC(SYS_YIELD, 0);
    Scheduler::instance().schedule();
}

void SyscallDispatcher::handle_sleep(InterruptFrame* frame) {
    AUDIT_HOOK_SVC(SYS_SLEEP, 0);
    constexpr uint32_t MAX_SLEEP_MS = 600000u;
    const uint32_t ms = frame->arg0;
    if (ms > MAX_SLEEP_MS) {
        uart_puts("[Kernel] SYS_SLEEP: duration out of range\n");
        return;
    }
    Scheduler::instance().sleep_ms(ms);
}

void SyscallDispatcher::handle_cap_derive(InterruptFrame* frame) {
    AUDIT_HOOK_SVC(SYS_CAP_DERIVE, 0);
    TaskControlBlock* cur = Scheduler::instance().get_current_tcb();
    if (!cur)
        return;
    uint32_t src = frame->arg0;
    uint32_t dst = frame->arg1;
    uint32_t rights = frame->arg2;
    if (!CSpace::cap_derive(cur, src, dst, rights)) {
        uart_puts("[Kernel] SYS_CAP_DERIVE: failed\n");
        Scheduler::instance().set_task_state(cur->scheduler.id, TaskState::Terminated);
        Scheduler::instance().schedule();
    }
}

void SyscallDispatcher::handle_cap_mint(InterruptFrame* frame) {
    AUDIT_HOOK_SVC(SYS_CAP_MINT, 0);
    TaskControlBlock* cur = Scheduler::instance().get_current_tcb();
    if (!cur)
        return;
    uint32_t src = frame->arg0;
    uint32_t dst = frame->arg1;
    uint32_t rights = frame->arg2;
    uint32_t badge = frame->arg3;
    if (!CSpace::cap_mint(cur, src, dst, rights, badge)) {
        uart_puts("[Kernel] SYS_CAP_MINT: failed\n");
        Scheduler::instance().set_task_state(cur->scheduler.id, TaskState::Terminated);
        Scheduler::instance().schedule();
    }
}

void SyscallDispatcher::handle_cap_revoke(InterruptFrame* frame) {
    AUDIT_HOOK_SVC(SYS_CAP_REVOKE, 0);
    TaskControlBlock* cur = Scheduler::instance().get_current_tcb();
    if (!cur)
        return;
    uint32_t slot = frame->arg0;
    if (!CSpace::cap_revoke(cur, slot)) {
        uart_puts("[Kernel] SYS_CAP_REVOKE: failed\n");
        Scheduler::instance().set_task_state(cur->scheduler.id, TaskState::Terminated);
        Scheduler::instance().schedule();
    }
}

void SyscallDispatcher::handle_cap_grant(InterruptFrame* frame) {
    AUDIT_HOOK_SVC(SYS_CAP_GRANT, 0);
    TaskControlBlock* cur = Scheduler::instance().get_current_tcb();
    if (!cur)
        return;
    const CapGrantDesc* desc = reinterpret_cast<const CapGrantDesc*>(frame->arg0);

    if (!SyscallValidator::validate_user_ptr(desc, sizeof(CapGrantDesc), cur, false)) {
        uart_puts("[Kernel] SYS_CAP_GRANT: invalid desc ptr\n");
        return;
    }

    TaskControlBlock* target_tcb = Scheduler::instance().get_task_by_id(desc->target_task_id);
    if (!target_tcb) {
        uart_puts("[Kernel] SYS_CAP_GRANT: target task not found\n");
        return;
    }

    if (!CSpace::cap_grant(cur, target_tcb, desc->src_slot, desc->dst_slot, desc->new_rights, desc->badge)) {
        uart_puts("[Kernel] SYS_CAP_GRANT: failed\n");
        Scheduler::instance().set_task_state(cur->scheduler.id, TaskState::Terminated);
        Scheduler::instance().schedule();
    }
}

void SyscallDispatcher::handle_cap_delete(InterruptFrame* frame) {
    AUDIT_HOOK_SVC(SYS_CAP_DELETE, 0);
    TaskControlBlock* cur = Scheduler::instance().get_current_tcb();
    if (!cur)
        return;
    uint32_t slot = frame->arg0;
    CSpace::cap_delete(cur, slot);
}

void SyscallDispatcher::handle_kill(InterruptFrame* frame) {
    AUDIT_HOOK_SVC(SYS_KILL, 0);
    TaskControlBlock* cur = Scheduler::instance().get_current_tcb();
    if (!cur)
        return;
    uint32_t target_id = frame->arg0;
    int sig = static_cast<int>(frame->arg1);

    if (sig < 1 || sig >= 16) {
        frame->arg0 = static_cast<uint32_t>(-1);
        return;
    }

    TaskControlBlock* target = Scheduler::instance().get_task_by_id(target_id);
    if (!target) {
        frame->arg0 = static_cast<uint32_t>(-1);
        return;
    }

    // 写入目标任务的待处理信号位图
    target->security.pending_signals |= (1U << sig);

    if (target->scheduler.state == TaskState::Sleeping || target->scheduler.state == TaskState::Blocked_On_Notify) {
        Scheduler::instance().set_task_state(target->scheduler.id, TaskState::Ready);
    }

    frame->arg0 = 0;
}

void SyscallDispatcher::handle_sigaction(InterruptFrame* frame) {
    AUDIT_HOOK_SVC(SYS_SIGACTION, 0);
    TaskControlBlock* cur = Scheduler::instance().get_current_tcb();
    if (!cur)
        return;
    int sig = static_cast<int>(frame->arg0);
    const SignalAction* act = reinterpret_cast<const SignalAction*>(frame->arg1);
    SignalAction* oldact = reinterpret_cast<SignalAction*>(frame->arg2);

    if (sig < 1 || sig >= 16 || sig == SIGKILL) {
        frame->arg0 = static_cast<uint32_t>(-1);
        return;
    }

    if (oldact) {
        if (SyscallValidator::validate_user_ptr(oldact, sizeof(SignalAction), cur, true)) {
            *oldact = cur->security.sig_actions[sig];
        } else {
            frame->arg0 = static_cast<uint32_t>(-1);
            return;
        }
    }

    if (act) {
        if (SyscallValidator::validate_user_ptr(act, sizeof(SignalAction), cur, false)) {
            cur->security.sig_actions[sig] = *act;
        } else {
            frame->arg0 = static_cast<uint32_t>(-1);
            return;
        }
    }

    frame->arg0 = 0;
}

void SyscallDispatcher::handle_sigprocmask(InterruptFrame* frame) {
    AUDIT_HOOK_SVC(SYS_SIGPROCMASK, 0);
    TaskControlBlock* cur = Scheduler::instance().get_current_tcb();
    if (!cur)
        return;
    int how = static_cast<int>(frame->arg0);
    const uint32_t* set = reinterpret_cast<const uint32_t*>(frame->arg1);
    uint32_t* oldset = reinterpret_cast<uint32_t*>(frame->arg2);

    if (oldset) {
        if (SyscallValidator::validate_user_ptr(oldset, sizeof(uint32_t), cur, true)) {
            *oldset = cur->security.signal_mask;
        } else {
            frame->arg0 = static_cast<uint32_t>(-1);
            return;
        }
    }

    if (set) {
        if (SyscallValidator::validate_user_ptr(set, sizeof(uint32_t), cur, false)) {
            uint32_t new_mask = *set;
            sigdelset(&new_mask, SIGKILL);

            if (how == SIG_BLOCK) {
                cur->security.signal_mask |= new_mask;
            } else if (how == SIG_UNBLOCK) {
                cur->security.signal_mask &= ~new_mask;
            } else if (how == SIG_SETMASK) {
                cur->security.signal_mask = new_mask;
            } else {
                frame->arg0 = static_cast<uint32_t>(-1);
                return;
            }
        } else {
            frame->arg0 = static_cast<uint32_t>(-1);
            return;
        }
    }

    frame->arg0 = 0;
}

void SyscallDispatcher::handle_ipc_call(InterruptFrame* frame) {
    AUDIT_HOOK_SVC(SYS_IPC_CALL, 0);
    TaskControlBlock* cur = Scheduler::instance().get_current_tcb();
    if (!cur)
        return;
    uint32_t cap_id = frame->arg0;
    void* msg = reinterpret_cast<void*>(frame->arg1);
    uint32_t len = frame->arg2;
    const IpcReplyDesc* desc = reinterpret_cast<const IpcReplyDesc*>(frame->arg3);

    if (!SyscallValidator::validate_user_ptr(desc, sizeof(IpcReplyDesc), cur, false)) {
        uart_puts("[Kernel] SYS_IPC_CALL: invalid desc ptr\n");
        frame->arg0 = static_cast<uint32_t>(IpcStatus::Invalid);
        return;
    }

    void* reply_buf = desc->buf;
    uint32_t max_reply_len = desc->max_len;
    uint32_t timeout_ms = desc->timeout_ms;

    if (len > 0 && !SyscallValidator::validate_user_ptr(msg, len, cur, false)) {
        uart_puts("[Kernel] SYS_IPC_CALL: invalid msg ptr\n");
        frame->arg0 = static_cast<uint32_t>(IpcStatus::Invalid);
        return;
    }
    if (max_reply_len > 0 && !SyscallValidator::validate_user_ptr(reply_buf, max_reply_len, cur, true)) {
        uart_puts("[Kernel] SYS_IPC_CALL: invalid reply_buf ptr\n");
        frame->arg0 = static_cast<uint32_t>(IpcStatus::Invalid);
        return;
    }

    uint32_t timeout_ticks = IPC_TIMEOUT_INFINITE;
    if (timeout_ms == 0) {
        timeout_ticks = IPC_NONBLOCK;
    } else if (timeout_ms != IPC_TIMEOUT_INFINITE) {
        timeout_ticks = static_cast<uint32_t>((static_cast<uint64_t>(timeout_ms) * Scheduler::TICK_RATE_HZ + 999u) / 1000u);
    }

    int res = KernelIpc::sys_ipc_call(cur, cap_id, msg, len, reply_buf, max_reply_len, timeout_ticks);
    frame->arg0 = static_cast<uint32_t>(res);
}

void SyscallDispatcher::handle_ipc_receive(InterruptFrame* frame) {
    AUDIT_HOOK_SVC(SYS_IPC_RECEIVE, 0);
    TaskControlBlock* cur = Scheduler::instance().get_current_tcb();
    if (!cur) {
        frame->arg0 = static_cast<uint32_t>(IpcStatus::Invalid);
        return;
    }
    uint32_t cap_id = frame->arg0;
    void* msg_buf = reinterpret_cast<void*>(frame->arg1);
    uint32_t max_len = frame->arg2;
    uint32_t* out_sender_id = reinterpret_cast<uint32_t*>(frame->arg3);

    if (max_len > 0 && !SyscallValidator::validate_user_ptr(msg_buf, max_len, cur, true)) {
        uart_puts("[Kernel] SYS_IPC_RECEIVE: invalid msg_buf ptr\n");
        frame->arg0 = static_cast<uint32_t>(IpcStatus::Invalid);
        return;
    }
    if (out_sender_id &&
        !SyscallValidator::validate_user_ptr(out_sender_id, sizeof(uint32_t), cur, true)) {
        uart_puts("[Kernel] SYS_IPC_RECEIVE: invalid out_sender_id ptr\n");
        frame->arg0 = static_cast<uint32_t>(IpcStatus::Invalid);
        return;
    }

    int res = KernelIpc::sys_ipc_receive(cur, cap_id, msg_buf, max_len, out_sender_id);
    frame->arg0 = static_cast<uint32_t>(res);
}

void SyscallDispatcher::handle_ipc_reply(InterruptFrame* frame) {
    AUDIT_HOOK_SVC(SYS_IPC_REPLY, 0);
    TaskControlBlock* cur = Scheduler::instance().get_current_tcb();
    if (!cur) {
        frame->arg0 = static_cast<uint32_t>(IpcStatus::Invalid);
        return;
    }
    uint32_t sender_id = frame->arg0;
    void* reply_msg = reinterpret_cast<void*>(frame->arg1);
    uint32_t len = frame->arg2;

    if (len > 0 && !SyscallValidator::validate_user_ptr(reply_msg, len, cur, false)) {
        uart_puts("[Kernel] SYS_IPC_REPLY: invalid reply_msg ptr\n");
        frame->arg0 = static_cast<uint32_t>(IpcStatus::Invalid);
        return;
    }

    int res = KernelIpc::sys_ipc_reply(cur, sender_id, reply_msg, len);
    frame->arg0 = static_cast<uint32_t>(res);
}

void SyscallDispatcher::handle_dev_open(InterruptFrame* frame) {
    AUDIT_HOOK_SVC(SYS_DEV_OPEN, 0);
    TaskControlBlock* cur = Scheduler::instance().get_current_tcb();
    if (!cur) {
        frame->arg0 = static_cast<uint32_t>(-1);
        return;
    }

    const DevOpenDesc* desc = reinterpret_cast<const DevOpenDesc*>(frame->arg0);

    if (!SyscallValidator::validate_user_ptr(desc, sizeof(DevOpenDesc), cur, false)) {
        frame->arg0 = static_cast<uint32_t>(-2);
        return;
    }

    if (!desc->name || !SyscallValidator::validate_user_ptr(desc->name, 1, cur, false)) {
        frame->arg0 = static_cast<uint32_t>(-2);
        return;
    }

    int res = DeviceRegistry::instance().open_device(cur, desc->name, desc->dst_slot, desc->rights);
    frame->arg0 = static_cast<uint32_t>(res);
}

void SyscallDispatcher::handle_dev_read(InterruptFrame* frame) {
    AUDIT_HOOK_SVC(SYS_DEV_READ, 0);
    TaskControlBlock* cur = Scheduler::instance().get_current_tcb();
    if (!cur) {
        frame->arg0 = static_cast<uint32_t>(-1);
        return;
    }

    const DevIoDesc* desc = reinterpret_cast<const DevIoDesc*>(frame->arg0);

    if (!SyscallValidator::validate_user_ptr(desc, sizeof(DevIoDesc), cur, false)) {
        frame->arg0 = static_cast<uint32_t>(-2);
        return;
    }

    if (desc->len > 0 && !SyscallValidator::validate_user_ptr(desc->buf, desc->len, cur, true)) {
        frame->arg0 = static_cast<uint32_t>(-2);
        return;
    }

    int res = DeviceRegistry::instance().device_read(cur, desc->cap_slot, static_cast<char*>(desc->buf),
                                                     static_cast<int>(desc->len), static_cast<int>(desc->offset));
    frame->arg0 = static_cast<uint32_t>(res);
}

void SyscallDispatcher::handle_dev_write(InterruptFrame* frame) {
    AUDIT_HOOK_SVC(SYS_DEV_WRITE, 0);
    TaskControlBlock* cur = Scheduler::instance().get_current_tcb();
    if (!cur) {
        frame->arg0 = static_cast<uint32_t>(-1);
        return;
    }

    const DevIoDesc* desc = reinterpret_cast<const DevIoDesc*>(frame->arg0);

    if (!SyscallValidator::validate_user_ptr(desc, sizeof(DevIoDesc), cur, false)) {
        frame->arg0 = static_cast<uint32_t>(-2);
        return;
    }

    if (desc->len > 0 && !SyscallValidator::validate_user_ptr(desc->buf, desc->len, cur, false)) {
        frame->arg0 = static_cast<uint32_t>(-2);
        return;
    }

    int res = DeviceRegistry::instance().device_write(cur, desc->cap_slot, static_cast<const char*>(desc->buf),
                                                      static_cast<int>(desc->len), static_cast<int>(desc->offset));
    frame->arg0 = static_cast<uint32_t>(res);
}

void SyscallDispatcher::handle_dev_ioctl(InterruptFrame* frame) {
    AUDIT_HOOK_SVC(SYS_DEV_IOCTL, 0);
    TaskControlBlock* cur = Scheduler::instance().get_current_tcb();
    if (!cur) {
        frame->arg0 = static_cast<uint32_t>(-1);
        return;
    }

    const DevIoctlDesc* desc = reinterpret_cast<const DevIoctlDesc*>(frame->arg0);

    if (!SyscallValidator::validate_user_ptr(desc, sizeof(DevIoctlDesc), cur, false)) {
        frame->arg0 = static_cast<uint32_t>(-2);
        return;
    }

    int res = DeviceRegistry::instance().device_ioctl(cur, desc->cap_slot, static_cast<int>(desc->request), desc->arg);
    frame->arg0 = static_cast<uint32_t>(res);
}

void SyscallDispatcher::handle_dev_register(InterruptFrame* frame) {
    AUDIT_HOOK_SVC(SYS_DEV_REGISTER, 0);
    TaskControlBlock* cur = Scheduler::instance().get_current_tcb();
    if (!cur) {
        frame->arg0 = static_cast<uint32_t>(-1);
        return;
    }

    Device* dev = reinterpret_cast<Device*>(frame->arg0);
    uint32_t rights = frame->arg1;
    if (!dev) {
        frame->arg0 = static_cast<uint32_t>(-2);
        return;
    }

    bool ok = DeviceRegistry::instance().register_device(dev, rights);
    frame->arg0 = ok ? 0 : static_cast<uint32_t>(-1);
}

void SyscallDispatcher::handle_unknown(InterruptFrame* /*frame*/) {
    uart_puts("[Kernel] Unknown SVC call\n");
    TaskControlBlock* cur = Scheduler::instance().get_current_tcb();
    if (cur) {
        Scheduler::instance().set_task_state(cur->scheduler.id, TaskState::Terminated);
        Scheduler::instance().schedule();
    }
}

} // namespace kernel
} // namespace auroraos
