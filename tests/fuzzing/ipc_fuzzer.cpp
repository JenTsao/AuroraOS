// =============================================================================
// tests/fuzzing/ipc_fuzzer.cpp — IPC Endpoint 状态机模糊测试 harness
//
// 靶子：auroraos::kernel::Endpoint 的 call / nb_call / receive / nb_receive /
//       reply / cancel_waiter 全状态机（kernel/core/ipc.cpp），覆盖 fast-path
//       投递、阻塞入队、非阻塞 WouldBlock、Label 选择性接收、seL4 Badge 传递、
//       PIP 优先级继承、等待者取消与析构清理等分支。
//
// 诚实性说明（重要）：内核 IPC 层信任调用方传入的 len/max_len 如实描述其缓冲区，
// 真正的边界校验在 syscall 层由 SyscallValidator::validate_user_ptr 完成
// (syscall_dispatcher.cpp:382/387/420)。因此本 harness **不**谎报缓冲区长度：
// 所有 len/max_len 均由真实静态缓冲区大小封顶，模糊的是消息内容、Label、Badge
// 与调用时序/模式。这样 ASAN 若命中，指向的是内核状态机缺陷而非 harness 造假。
// 原 ipc_fuzzer.cpp 仅 memcpy 后即 return，无任何靶子调用——本文件用真实状态机重写。
//
// 由 fuzzer_main.cpp 的 LLVMFuzzerTestOneInput 统一 dispatch 调用。
// =============================================================================

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../kernel/task/task.hpp"
#include "../../kernel/core/ipc.hpp"

using auroraos::kernel::Endpoint;
using auroraos::kernel::IpcStatus;

// 真实静态缓冲区：所有传给 IPC 的 len/max_len 都不超过其容量，保证诚实性。
static constexpr uint32_t kBufCap = 5120u;
static uint8_t g_send_buf[kBufCap];
static uint8_t g_recv_buf[kBufCap];
static uint8_t g_reply_buf[kBufCap];
static uint32_t g_sender_stack[128];
static uint32_t g_receiver_stack[128];

extern void aurora_fuzz_ipc(const uint8_t* data, size_t size) {
    if (size < 8)
        return;

    // 每轮迭代重置调度器并新建 2 个任务，避免 16 槽位被阻塞任务占满，
    // 也保证 Endpoint 局部对象在迭代结束时析构（cancel_all）不跨轮持有悬挂指针。
    Scheduler::instance().init();
    TaskControlBlock* sender =
        Scheduler::instance().create_task([]() {}, g_sender_stack, sizeof(g_sender_stack));
    TaskControlBlock* receiver =
        Scheduler::instance().create_task([]() {}, g_receiver_stack, sizeof(g_receiver_stack));
    if (!sender || !receiver)
        return;

    // 用前几个字节派生控制参数，其余作为消息内容。
    size_t p = 0;
    const uint32_t label = static_cast<uint32_t>(data[p]) | (static_cast<uint32_t>(data[p + 1]) << 8);
    p += 2;
    const uint32_t badge = static_cast<uint32_t>(data[p]) | (static_cast<uint32_t>(data[p + 1]) << 8) |
                           (static_cast<uint32_t>(data[p + 2]) << 16) | (static_cast<uint32_t>(data[p + 3]) << 24);
    p += 4;
    const uint8_t mode = data[p++];       // 0=block,1=nonblock,2=timeout
    const uint8_t action = data[p++];     // 选择要驱动的状态机路径

    // 真实长度：由剩余字节数派生，但严格封顶到缓冲区容量，绝不谎报。
    const size_t remaining = size - p;
    const size_t copy_n = remaining < kBufCap ? remaining : kBufCap;
    if (copy_n > 0)
        memcpy(g_send_buf, data + p, copy_n);

    const uint32_t msg_len = static_cast<uint32_t>(copy_n);           // 诚实：== 真实填充字节数
    const uint32_t recv_cap = static_cast<uint32_t>(mode & 0x01u ? copy_n : (copy_n >> 1));
    const uint32_t reply_cap = static_cast<uint32_t>((badge & 0x01u) ? msg_len : 16u);

    Endpoint ep;
    switch (action & 0x03u) {
    case 0: {
        // receiver 先阻塞接收，sender 随后 call —— fast-path 投递 + reply 回填。
        ep.receive(receiver, g_recv_buf, recv_cap, auroraos::kernel::IPC_TIMEOUT_INFINITE, label);
        IpcStatus st = ep.call(sender, g_send_buf, msg_len, g_reply_buf, reply_cap,
                               auroraos::kernel::IPC_TIMEOUT_INFINITE, badge);
        if (st == IpcStatus::Blocked) {
            ep.reply(receiver, sender->scheduler.id, g_send_buf, msg_len);
        } else if (st == IpcStatus::Ok) {
            // fast-path：receiver 已就绪，直接回复唤醒 sender。
            ep.reply(receiver, receiver->ipc.sender_id, g_send_buf, msg_len);
        }
        break;
    }
    case 1: {
        // sender 先非阻塞 call（无接收方 → WouldBlock），再 receive 消费。
        ep.nb_call(sender, g_send_buf, msg_len, g_reply_buf, reply_cap, badge);
        ep.receive(receiver, g_recv_buf, recv_cap, auroraos::kernel::IPC_TIMEOUT_INFINITE, label);
        break;
    }
    case 2: {
        // 非阻塞 receive（空队列 → WouldBlock），随后带超时的 call。
        ep.nb_receive(receiver, g_recv_buf, recv_cap, label);
        ep.call(sender, g_send_buf, msg_len, g_reply_buf, reply_cap,
                static_cast<uint32_t>(label & 0xFFu), badge);
        break;
    }
    default: {
        // 取消路径：sender 阻塞后取消，验证等待队列清理无悬挂。
        ep.call(sender, g_send_buf, msg_len, g_reply_buf, reply_cap, auroraos::kernel::IPC_TIMEOUT_INFINITE, badge);
        ep.cancel_waiter(sender, auroraos::kernel::IpcStatus::Timeout);
        break;
    }
    }

    // 迭代结束：局部 Endpoint ep 析构触发 cancel_all，清理所有挂起等待者，
    // 因此不会有跨迭代的悬挂指针残留。
}
