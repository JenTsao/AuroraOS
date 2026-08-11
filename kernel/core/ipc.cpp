#include "ipc.hpp"
#include "task.hpp"

namespace auroraos {
namespace kernel {

static constexpr uint32_t MAX_IPC_MSG_SIZE = 4096; // 4KB hard limit to prevent IRQ blocking DoS


void Endpoint::call(TaskControlBlock* sender, void* msg, uint32_t len, void* reply_buf, uint32_t max_reply_len) {
    IrqGuard guard; 
    sender->ipc_reply_buf = reply_buf;
    sender->ipc_max_len = max_reply_len;

    if (!recv_queue_.empty()) {
        TaskControlBlock* receiver = recv_queue_.dequeue();
        
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
        sender->ipc_receiver_id = receiver->id; // 璁板綍璋佹湁鏉冨洖澶?
        receiver->ipc_state = IpcState::Ready;
        Scheduler::instance().push_ready(receiver->id);

        sender->ipc_state = IpcState::ReplyBlocked;
    } else {
        sender->ipc_msg_buf = msg;
        sender->ipc_msg_len = len;
        sender->ipc_state = IpcState::Sending;
        send_queue_.enqueue(sender);
    }
}

void Endpoint::receive(TaskControlBlock* receiver, void* msg_buf, uint32_t max_len) {
    IrqGuard guard;
    receiver->ipc_msg_buf = msg_buf;
    receiver->ipc_max_len = max_len;

    if (!send_queue_.empty()) {
        TaskControlBlock* sender = send_queue_.dequeue();
        
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
        sender->ipc_receiver_id = receiver->id; // 璁板綍璋佹湁鏉冨洖澶?
        // Receiver does not block, it stays Ready since it immediately got the message
        
        sender->ipc_state = IpcState::ReplyBlocked;
    } else {
        receiver->ipc_state = IpcState::Receiving;
        recv_queue_.enqueue(receiver);
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
