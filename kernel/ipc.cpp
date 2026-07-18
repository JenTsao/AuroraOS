#include "ipc.hpp"
#include "task.hpp"

namespace auroraos {
namespace kernel {

static constexpr uint32_t MAX_IPC_MSG_SIZE = 4096; // 4KB hard limit to prevent IRQ blocking DoS


void Endpoint::enqueue_sender(TaskControlBlock* tcb) {
    tcb->ipc_blocked_next = nullptr;
    if (!send_tail_) {
        send_head_ = send_tail_ = tcb;
    } else {
        send_tail_->ipc_blocked_next = tcb;
        send_tail_ = tcb;
    }
}

TaskControlBlock* Endpoint::dequeue_sender() {
    if (!send_head_) return nullptr;
    TaskControlBlock* tcb = send_head_;
    send_head_ = tcb->ipc_blocked_next;
    if (!send_head_) send_tail_ = nullptr;
    return tcb;
}

void Endpoint::enqueue_receiver(TaskControlBlock* tcb) {
    tcb->ipc_blocked_next = nullptr;
    if (!recv_tail_) {
        recv_head_ = recv_tail_ = tcb;
    } else {
        recv_tail_->ipc_blocked_next = tcb;
        recv_tail_ = tcb;
    }
}

TaskControlBlock* Endpoint::dequeue_receiver() {
    if (!recv_head_) return nullptr;
    TaskControlBlock* tcb = recv_head_;
    recv_head_ = tcb->ipc_blocked_next;
    if (!recv_head_) recv_tail_ = nullptr;
    return tcb;
}

void Endpoint::call(TaskControlBlock* sender, void* msg, uint32_t len, void* reply_buf, uint32_t max_reply_len) {
    IrqGuard guard; 
    sender->ipc_reply_buf = reply_buf;
    sender->ipc_max_len = max_reply_len;

    if (recv_head_) {
        TaskControlBlock* receiver = dequeue_receiver();
        
        uint32_t copy_len = (len < receiver->ipc_max_len) ? len : receiver->ipc_max_len;
        if (copy_len > MAX_IPC_MSG_SIZE) copy_len = MAX_IPC_MSG_SIZE;
        
        bool did_copy = (copy_len > 0 && msg && receiver->ipc_msg_buf);
        if (did_copy) {
            char* src = static_cast<char*>(msg);
            char* dst = static_cast<char*>(receiver->ipc_msg_buf);
            for(uint32_t i=0; i<copy_len; ++i) dst[i] = src[i];
        }
        
        receiver->ipc_msg_len = did_copy ? copy_len : 0;
        receiver->ipc_sender_id = sender->id;
        sender->ipc_receiver_id = receiver->id; // 记录谁有权回复
        receiver->ipc_state = IpcState::Ready;
        Scheduler::instance().push_ready(receiver->id);

        sender->ipc_state = IpcState::ReplyBlocked;
    } else {
        sender->ipc_msg_buf = msg;
        sender->ipc_msg_len = len;
        sender->ipc_state = IpcState::Sending;
        enqueue_sender(sender);
    }
}

void Endpoint::receive(TaskControlBlock* receiver, void* msg_buf, uint32_t max_len) {
    IrqGuard guard;
    receiver->ipc_msg_buf = msg_buf;
    receiver->ipc_max_len = max_len;

    if (send_head_) {
        TaskControlBlock* sender = dequeue_sender();
        
        uint32_t copy_len = (sender->ipc_msg_len < max_len) ? sender->ipc_msg_len : max_len;
        if (copy_len > MAX_IPC_MSG_SIZE) copy_len = MAX_IPC_MSG_SIZE;
        
        bool did_copy = (copy_len > 0 && sender->ipc_msg_buf && msg_buf);
        if (did_copy) {
            char* src = static_cast<char*>(sender->ipc_msg_buf);
            char* dst = static_cast<char*>(msg_buf);
            for(uint32_t i=0; i<copy_len; ++i) dst[i] = src[i];
        }
        
        receiver->ipc_msg_len = did_copy ? copy_len : 0;
        receiver->ipc_sender_id = sender->id;
        sender->ipc_receiver_id = receiver->id; // 记录谁有权回复
        // Receiver does not block, it stays Ready since it immediately got the message
        
        sender->ipc_state = IpcState::ReplyBlocked;
    } else {
        receiver->ipc_state = IpcState::Receiving;
        enqueue_receiver(receiver);
    }
}

void Endpoint::reply(TaskControlBlock* receiver, uint32_t sender_id, void* reply_msg, uint32_t len) {
    IrqGuard guard;
    if (sender_id >= Scheduler::get_max_tasks()) return;
    
    TaskControlBlock* sender_ptr = Scheduler::instance().get_task_by_id(sender_id);
    if (!sender_ptr) return;
    TaskControlBlock& sender = *sender_ptr;
    
    if (sender.ipc_state == IpcState::ReplyBlocked && sender.ipc_receiver_id == receiver->id) {
        uint32_t copy_len = (len < sender.ipc_max_len) ? len : sender.ipc_max_len;
        if (copy_len > MAX_IPC_MSG_SIZE) copy_len = MAX_IPC_MSG_SIZE;
        
        bool did_copy = (copy_len > 0 && reply_msg && sender.ipc_reply_buf);
        if (did_copy) {
            char* src = static_cast<char*>(reply_msg);
            char* dst = static_cast<char*>(sender.ipc_reply_buf);
            for(uint32_t i=0; i<copy_len; ++i) dst[i] = src[i];
        }
        
        sender.ipc_msg_len = did_copy ? copy_len : 0;
        sender.ipc_state = IpcState::Ready;
        Scheduler::instance().push_ready(sender.id);
    }
}

} // namespace kernel
} // namespace auroraos
