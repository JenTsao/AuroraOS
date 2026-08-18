#ifndef SYSCALL_DISPATCHER_HPP
#define SYSCALL_DISPATCHER_HPP

#include <stdint.h>
#include "syscall.hpp"
#include "../task/task.hpp"
#include "../../boot/interrupts.hpp"

namespace auroraos {
namespace kernel {

class SyscallDispatcher {
public:
    using SyscallHandlerFn = void (*)(InterruptFrame* frame);

    // Initialize the syscall table
    static void init();

    // Dispatch a syscall
    static void dispatch(InterruptFrame* frame, uint8_t svc_number);

private:
    static SyscallHandlerFn syscall_table[256];

    // Handlers
    static void handle_print(InterruptFrame* frame);
    static void handle_yield(InterruptFrame* frame);
    static void handle_sleep(InterruptFrame* frame);
    static void handle_cap_derive(InterruptFrame* frame);
    static void handle_cap_mint(InterruptFrame* frame);
    static void handle_cap_revoke(InterruptFrame* frame);
    static void handle_cap_grant(InterruptFrame* frame);
    static void handle_cap_delete(InterruptFrame* frame);
    static void handle_kill(InterruptFrame* frame);
    static void handle_sigaction(InterruptFrame* frame);
    static void handle_sigprocmask(InterruptFrame* frame);
    static void handle_ipc_call(InterruptFrame* frame);
    static void handle_ipc_receive(InterruptFrame* frame);
    static void handle_ipc_reply(InterruptFrame* frame);
    static void handle_dev_open(InterruptFrame* frame);
    static void handle_dev_read(InterruptFrame* frame);
    static void handle_dev_write(InterruptFrame* frame);
    static void handle_dev_ioctl(InterruptFrame* frame);
    static void handle_dev_register(InterruptFrame* frame);
    static void handle_unknown(InterruptFrame* frame);
};

} // namespace kernel
} // namespace auroraos

#endif // SYSCALL_DISPATCHER_HPP
