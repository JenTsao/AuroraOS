// =============================================================================
// kernel/core/ipc.cpp
//
// 统一 IPC 端点消息传递与超时/非阻塞控制实现
// =============================================================================
#include "ipc.hpp"
#include "../task/task.hpp"

namespace auroraos {
namespace kernel {

static constexpr uint32_t MAX_IPC_MSG_SIZE = 4096; // 4KB 硬上限，防止长消息阻塞中断导致 DoS

IpcStatus Endpoint::call(TaskControlBlock* sender, void* msg, uint32_t len,
                         void* reply_buf, uint32_t max_reply_len,
                         uint32_t timeout_ticks) {
    if (!sender)
        return IpcStatus::Invalid;

    IrqGuard guard;

    sender->ipc.reply_buf = reply_buf;
    sender->ipc.max_len = max_reply_len;
    sender->ipc.status = IpcStatus::Ok;

    // 1. 若已有接收方就绪等待 (Fast-path 直接拷贝)
    if (!recv_queue_.empty()) {
        TaskControlBlock* receiver = recv_queue_.dequeue();

        uint32_t copy_len = (len < receiver->ipc.max_len) ? len : receiver->ipc.max_len;
        if (copy_len > MAX_IPC_MSG_SIZE)
            copy_len = MAX_IPC_MSG_SIZE;

        bool did_copy = (copy_len > 0 && msg && receiver->ipc.msg_buf);
        if (did_copy) {
            char* src = static_cast<char*>(msg);
            char* dst = static_cast<char*>(receiver->ipc.msg_buf);
            for (uint32_t i = 0; i < copy_len; ++i)
                dst[i] = src[i];
        }

        receiver->ipc.msg_len = did_copy ? copy_len : 0;
        receiver->ipc.sender_id = sender->scheduler.id;
        receiver->ipc.state = IpcState::Ready;
        receiver->ipc.status = IpcStatus::Ok;
        receiver->ipc.waiting_endpoint = nullptr;
        receiver->scheduler.sleep_ticks = 0;

        sender->ipc.receiver_id = receiver->scheduler.id; // 记录由该 receiver 应答

        Scheduler::instance().push_ready(receiver->scheduler.id);

        if (timeout_ticks == IPC_NONBLOCK) {
            // 非阻塞调用：如果消息已投递，但不能阻塞等待应答，直接保持 Ready
            sender->ipc.state = IpcState::Ready;
            sender->ipc.waiting_endpoint = nullptr;
            sender->scheduler.sleep_ticks = 0;
            return IpcStatus::Ok;
        }

        // 进入等待对端应答状态 (ReplyBlocked)
        sender->ipc.state = IpcState::ReplyBlocked;
        sender->ipc.waiting_endpoint = this;
        sender->scheduler.sleep_ticks = (timeout_ticks == IPC_TIMEOUT_INFINITE) ? 0 : timeout_ticks;
        return IpcStatus::Ok;
    }

    // 2. 若当前无接收方在等待
    if (timeout_ticks == IPC_NONBLOCK) {
        // 非阻塞模式下，对端未就绪立即返回 WouldBlock
        sender->ipc.state = IpcState::Ready;
        sender->ipc.status = IpcStatus::WouldBlock;
        sender->ipc.waiting_endpoint = nullptr;
        sender->scheduler.sleep_ticks = 0;
        return IpcStatus::WouldBlock;
    }

    // 阻塞 / 带超时入队等待接收方
    sender->ipc.msg_buf = msg;
    sender->ipc.msg_len = len;
    sender->ipc.state = IpcState::Sending;
    sender->ipc.waiting_endpoint = this;
    sender->scheduler.sleep_ticks = (timeout_ticks == IPC_TIMEOUT_INFINITE) ? 0 : timeout_ticks;
    send_queue_.enqueue(sender);
    return IpcStatus::Ok;
}

IpcStatus Endpoint::receive(TaskControlBlock* receiver, void* msg_buf, uint32_t max_len,
                            uint32_t timeout_ticks) {
    if (!receiver)
        return IpcStatus::Invalid;

    IrqGuard guard;

    receiver->ipc.msg_buf = msg_buf;
    receiver->ipc.max_len = max_len;
    receiver->ipc.status = IpcStatus::Ok;

    // 1. 若已有发送方就绪等待 (Fast-path 接收消息)
    if (!send_queue_.empty()) {
        TaskControlBlock* sender = send_queue_.dequeue();

        uint32_t copy_len = (sender->ipc.msg_len < max_len) ? sender->ipc.msg_len : max_len;
        if (copy_len > MAX_IPC_MSG_SIZE)
            copy_len = MAX_IPC_MSG_SIZE;

        bool did_copy = (copy_len > 0 && sender->ipc.msg_buf && msg_buf);
        if (did_copy) {
            char* src = static_cast<char*>(sender->ipc.msg_buf);
            char* dst = static_cast<char*>(msg_buf);
            for (uint32_t i = 0; i < copy_len; ++i)
                dst[i] = src[i];
        }

        receiver->ipc.msg_len = did_copy ? copy_len : 0;
        receiver->ipc.sender_id = sender->scheduler.id;
        receiver->ipc.state = IpcState::Ready;
        receiver->ipc.status = IpcStatus::Ok;
        receiver->ipc.waiting_endpoint = nullptr;
        receiver->scheduler.sleep_ticks = 0;

        sender->ipc.receiver_id = receiver->scheduler.id;
        sender->ipc.state = IpcState::ReplyBlocked;
        return IpcStatus::Ok;
    }

    // 2. 若当前无发送方在等待
    if (timeout_ticks == IPC_NONBLOCK) {
        receiver->ipc.state = IpcState::Ready;
        receiver->ipc.status = IpcStatus::WouldBlock;
        receiver->ipc.waiting_endpoint = nullptr;
        receiver->scheduler.sleep_ticks = 0;
        return IpcStatus::WouldBlock;
    }

    // 阻塞 / 带超时入队等待发送方
    receiver->ipc.state = IpcState::Receiving;
    receiver->ipc.waiting_endpoint = this;
    receiver->scheduler.sleep_ticks = (timeout_ticks == IPC_TIMEOUT_INFINITE) ? 0 : timeout_ticks;
    recv_queue_.enqueue(receiver);
    return IpcStatus::Ok;
}

IpcStatus Endpoint::reply(TaskControlBlock* receiver, uint32_t sender_id, void* reply_msg, uint32_t len) {
    if (!receiver)
        return IpcStatus::Invalid;

    IrqGuard guard;

    if (sender_id >= static_cast<uint32_t>(Scheduler::get_max_tasks()))
        return IpcStatus::ReceiverDead;

    TaskControlBlock* sender_ptr = Scheduler::instance().get_task_by_id(sender_id);
    if (!sender_ptr)
        return IpcStatus::ReceiverDead;

    TaskControlBlock& sender = *sender_ptr;

    if (sender.ipc.state == IpcState::ReplyBlocked && sender.ipc.receiver_id == receiver->scheduler.id) {
        uint32_t copy_len = (len < sender.ipc.max_len) ? len : sender.ipc.max_len;
        if (copy_len > MAX_IPC_MSG_SIZE)
            copy_len = MAX_IPC_MSG_SIZE;

        bool did_copy = (copy_len > 0 && reply_msg && sender.ipc.reply_buf);
        if (did_copy) {
            char* src = static_cast<char*>(reply_msg);
            char* dst = static_cast<char*>(sender.ipc.reply_buf);
            for (uint32_t i = 0; i < copy_len; ++i)
                dst[i] = src[i];
        }

        sender.ipc.msg_len = did_copy ? copy_len : 0;
        sender.ipc.state = IpcState::Ready;
        sender.ipc.status = IpcStatus::Ok;
        sender.ipc.waiting_endpoint = nullptr;
        sender.scheduler.sleep_ticks = 0;

        Scheduler::instance().push_ready(sender.scheduler.id);
        return IpcStatus::Ok;
    }

    return IpcStatus::Invalid;
}

void Endpoint::cancel_waiter(TaskControlBlock* task) {
    if (!task)
        return;

    IrqGuard guard;

    send_queue_.remove(task);
    recv_queue_.remove(task);

    task->ipc.waiting_endpoint = nullptr;
    task->ipc.state = IpcState::Ready;
    task->ipc.status = IpcStatus::Timeout;
    task->scheduler.sleep_ticks = 0;

    Scheduler::instance().push_ready(task->scheduler.id);
}

} // namespace kernel
} // namespace auroraos
