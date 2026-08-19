// =============================================================================
// kernel/core/ipc.cpp
//
// 统一 IPC 端点消息传递、Badge 认证、Label 过滤与生命周期安全清理实现
// =============================================================================
#include "ipc.hpp"
#include "../task/task.hpp"

namespace auroraos {
namespace kernel {

static constexpr uint32_t MAX_IPC_MSG_SIZE = 4096; // 4KB 硬上限，防止长消息阻塞中断导致 DoS

IpcStatus Endpoint::call(TaskControlBlock* sender, void* msg, uint32_t len,
                         void* reply_buf, uint32_t max_reply_len,
                         uint32_t timeout_ticks, uint32_t badge) {
    if (!sender)
        return IpcStatus::Invalid;

    IrqGuard guard;

    sender->ipc.reply_buf = reply_buf;
    sender->ipc.max_len = max_reply_len;
    sender->ipc.status = IpcStatus::Ok;
    sender->ipc.badge = badge;

    if (len >= sizeof(auroraos::kernel::IpcRawMessage) && msg) {
        const auto* hdr = static_cast<const auroraos::kernel::IpcRawMessage*>(msg);
        sender->ipc.msg_type = static_cast<uint32_t>(hdr->msg_type);
    } else {
        sender->ipc.msg_type = 0;
    }

    // 1. 若已有匹配该消息 Label 的接收方在等待 (Fast-path 传递)
    TaskControlBlock* receiver = recv_queue_.dequeue_matching_receiver(sender->ipc.msg_type);
    if (receiver) {
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
        receiver->ipc.badge = badge;                   // 传递 seL4 风格防伪 Badge
        receiver->ipc.msg_type = sender->ipc.msg_type; // 传递消息 Label / Type
        receiver->ipc.state = IpcState::Ready;
        receiver->ipc.status = IpcStatus::Ok;
        receiver->ipc.waiting_endpoint = nullptr;
        receiver->scheduler.sleep_ticks = 0;

        sender->ipc.receiver_id = receiver->scheduler.id; // 记录由该 receiver 应答

        // 正确同步调度器状态：从阻塞态唤醒 receiver
        Scheduler::instance().set_task_state(receiver->scheduler.id, TaskState::Ready);

        if (timeout_ticks == IPC_NONBLOCK) {
            // 非阻塞调用：如果消息已投递，但不能阻塞等待应答，直接保持 Ready
            sender->ipc.state = IpcState::Ready;
            sender->ipc.waiting_endpoint = nullptr;
            sender->scheduler.sleep_ticks = 0;
            return IpcStatus::Ok;
        }

        // 进入等待对端应答状态 (ReplyBlocked)，并从就绪队列摘除
        sender->ipc.state = IpcState::ReplyBlocked;
        sender->ipc.waiting_endpoint = this;
        sender->scheduler.sleep_ticks = (timeout_ticks == IPC_TIMEOUT_INFINITE) ? 0 : timeout_ticks;
        Scheduler::instance().set_task_state(sender->scheduler.id, TaskState::Blocked_On_Notify);
        return IpcStatus::Ok;
    }

    // 2. 若当前无匹配接收方在等待
    if (timeout_ticks == IPC_NONBLOCK) {
        // 非阻塞模式下立即返回 WouldBlock
        sender->ipc.state = IpcState::Ready;
        sender->ipc.status = IpcStatus::WouldBlock;
        sender->ipc.waiting_endpoint = nullptr;
        sender->scheduler.sleep_ticks = 0;
        return IpcStatus::WouldBlock;
    }

    // 阻塞 / 带超时入队等待接收方，并从就绪队列摘除
    sender->ipc.msg_buf = msg;
    sender->ipc.msg_len = len;
    sender->ipc.state = IpcState::Sending;
    sender->ipc.waiting_endpoint = this;
    sender->scheduler.sleep_ticks = (timeout_ticks == IPC_TIMEOUT_INFINITE) ? 0 : timeout_ticks;
    send_queue_.enqueue(sender);
    Scheduler::instance().set_task_state(sender->scheduler.id, TaskState::Blocked_On_Notify);
    return IpcStatus::Ok;
}

IpcStatus Endpoint::receive(TaskControlBlock* receiver, void* msg_buf, uint32_t max_len,
                            uint32_t timeout_ticks, uint32_t label_filter) {
    if (!receiver)
        return IpcStatus::Invalid;

    IrqGuard guard;

    receiver->ipc.msg_buf = msg_buf;
    receiver->ipc.max_len = max_len;
    receiver->ipc.status = IpcStatus::Ok;
    receiver->ipc.label_filter = label_filter;

    // 1. 若已有匹配 Label 过滤器的发送方在等待 (Fast-path 接收)
    TaskControlBlock* sender = send_queue_.dequeue_matching_sender(label_filter);
    if (sender) {
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
        receiver->ipc.badge = sender->ipc.badge;       // 传递发送方的 Badge 身份
        receiver->ipc.msg_type = sender->ipc.msg_type; // 传递发送方的消息 Label
        receiver->ipc.state = IpcState::Ready;
        receiver->ipc.status = IpcStatus::Ok;
        receiver->ipc.waiting_endpoint = nullptr;
        receiver->scheduler.sleep_ticks = 0;

        sender->ipc.receiver_id = receiver->scheduler.id;
        sender->ipc.state = IpcState::ReplyBlocked;
        // sender 从 Sending 转为 ReplyBlocked，保持阻塞（已在就绪队列外）
        // 若此前因某种原因仍在就绪队列，强制同步为阻塞态
        Scheduler::instance().set_task_state(sender->scheduler.id, TaskState::Blocked_On_Notify);
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

    // 阻塞 / 带超时入队等待发送方，并从就绪队列摘除
    receiver->ipc.state = IpcState::Receiving;
    receiver->ipc.waiting_endpoint = this;
    receiver->scheduler.sleep_ticks = (timeout_ticks == IPC_TIMEOUT_INFINITE) ? 0 : timeout_ticks;
    recv_queue_.enqueue(receiver);
    Scheduler::instance().set_task_state(receiver->scheduler.id, TaskState::Blocked_On_Notify);
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

        // 正确同步调度器状态：唤醒等待 reply 的 sender
        Scheduler::instance().set_task_state(sender.scheduler.id, TaskState::Ready);
        return IpcStatus::Ok;
    }

    return IpcStatus::Invalid;
}

void Endpoint::cancel_waiter(TaskControlBlock* task, IpcStatus reason) {
    if (!task)
        return;

    IrqGuard guard;

    send_queue_.remove(task);
    recv_queue_.remove(task);

    task->ipc.waiting_endpoint = nullptr;
    task->ipc.state = IpcState::Ready;
    task->ipc.status = reason;
    task->scheduler.sleep_ticks = 0;

    Scheduler::instance().set_task_state(task->scheduler.id, TaskState::Ready);
}

void Endpoint::cancel_all(IpcStatus reason) {
    IrqGuard guard;

    while (!send_queue_.empty()) {
        TaskControlBlock* sender = send_queue_.dequeue();
        if (sender) {
            sender->ipc.waiting_endpoint = nullptr;
            sender->ipc.state = IpcState::Ready;
            sender->ipc.status = reason;
            sender->ipc.blocked_next = nullptr;
            sender->scheduler.sleep_ticks = 0;
            Scheduler::instance().set_task_state(sender->scheduler.id, TaskState::Ready);
        }
    }

    while (!recv_queue_.empty()) {
        TaskControlBlock* receiver = recv_queue_.dequeue();
        if (receiver) {
            receiver->ipc.waiting_endpoint = nullptr;
            receiver->ipc.state = IpcState::Ready;
            receiver->ipc.status = reason;
            receiver->ipc.blocked_next = nullptr;
            receiver->scheduler.sleep_ticks = 0;
            Scheduler::instance().set_task_state(receiver->scheduler.id, TaskState::Ready);
        }
    }

    // 扫描可能处于 ReplyBlocked 等待本端点应答的任务
    int total_tasks = Scheduler::instance().get_task_count();
    for (int i = 0; i < total_tasks; i++) {
        TaskControlBlock* t = Scheduler::instance().get_task(i);
        if (t && t->ipc.waiting_endpoint == this) {
            t->ipc.waiting_endpoint = nullptr;
            t->ipc.state = IpcState::Ready;
            t->ipc.status = reason;
            t->ipc.blocked_next = nullptr;
            t->scheduler.sleep_ticks = 0;
            Scheduler::instance().set_task_state(t->scheduler.id, TaskState::Ready);
        }
    }
}

} // namespace kernel
} // namespace auroraos
