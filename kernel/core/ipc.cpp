#include "ipc.hpp"

#include "task.hpp"



namespace auroraos {

namespace kernel {



static constexpr uint32_t MAX_IPC_MSG_SIZE = 4096; // 4KB hard limit to prevent IRQ blocking DoS





void Endpoint::call(TaskControlBlock* sender, void* msg, uint32_t len, void* reply_buf, uint32_t max_reply_len) {

    IrqGuard guard; 

    sender->ipc.reply_buf = reply_buf;

    sender->ipc.max_len = max_reply_len;



    if (!recv_queue_.empty()) {

        TaskControlBlock* receiver = recv_queue_.dequeue();

        

        uint32_t copy_len = (len < receiver->ipc.max_len) ? len : receiver->ipc.max_len;

        if (copy_len > MAX_IPC_MSG_SIZE) copy_len = MAX_IPC_MSG_SIZE;

        

        bool did_copy = (copy_len > 0 && msg && receiver->ipc.msg_buf);

        if (did_copy) {

            char* src = static_cast<char*>(msg);

            char* dst = static_cast<char*>(receiver->ipc.msg_buf);

            for(uint32_t i=0; i<copy_len; ++i) dst[i] = src[i];

        }

        

        receiver->ipc.msg_len = did_copy ? copy_len : 0;

        receiver->ipc.sender_id = sender->scheduler.id;

        sender->ipc.receiver_id = receiver->scheduler.id; // 璁板綍璋佹湁鏉冨洖澶?

        receiver->ipc.state = IpcState::Ready;

        Scheduler::instance().push_ready(receiver->scheduler.id);



        sender->ipc.state = IpcState::ReplyBlocked;

    } else {

        sender->ipc.msg_buf = msg;

        sender->ipc.msg_len = len;

        sender->ipc.state = IpcState::Sending;

        send_queue_.enqueue(sender);

    }

}



void Endpoint::receive(TaskControlBlock* receiver, void* msg_buf, uint32_t max_len) {

    IrqGuard guard;

    receiver->ipc.msg_buf = msg_buf;

    receiver->ipc.max_len = max_len;



    if (!send_queue_.empty()) {

        TaskControlBlock* sender = send_queue_.dequeue();

        

        uint32_t copy_len = (sender->ipc.msg_len < max_len) ? sender->ipc.msg_len : max_len;

        if (copy_len > MAX_IPC_MSG_SIZE) copy_len = MAX_IPC_MSG_SIZE;

        

        bool did_copy = (copy_len > 0 && sender->ipc.msg_buf && msg_buf);

        if (did_copy) {

            char* src = static_cast<char*>(sender->ipc.msg_buf);

            char* dst = static_cast<char*>(msg_buf);

            for(uint32_t i=0; i<copy_len; ++i) dst[i] = src[i];

        }

        

        receiver->ipc.msg_len = did_copy ? copy_len : 0;

        receiver->ipc.sender_id = sender->scheduler.id;

        sender->ipc.receiver_id = receiver->scheduler.id; // 璁板綍璋佹湁鏉冨洖澶?

        // Receiver does not block, it stays Ready since it immediately got the message

        

        sender->ipc.state = IpcState::ReplyBlocked;

    } else {

        receiver->ipc.state = IpcState::Receiving;

        recv_queue_.enqueue(receiver);

    }

}



void Endpoint::reply(TaskControlBlock* receiver, uint32_t sender_id, void* reply_msg, uint32_t len) {

    IrqGuard guard;

    if (sender_id >= Scheduler::get_max_tasks()) return;

    

    TaskControlBlock* sender_ptr = Scheduler::instance().get_task_by_id(sender_id);

    if (!sender_ptr) return;

    TaskControlBlock& sender = *sender_ptr;

    

    if (sender.ipc.state == IpcState::ReplyBlocked && sender.ipc.receiver_id == receiver->scheduler.id) {

        uint32_t copy_len = (len < sender.ipc.max_len) ? len : sender.ipc.max_len;

        if (copy_len > MAX_IPC_MSG_SIZE) copy_len = MAX_IPC_MSG_SIZE;

        

        bool did_copy = (copy_len > 0 && reply_msg && sender.ipc.reply_buf);

        if (did_copy) {

            char* src = static_cast<char*>(reply_msg);

            char* dst = static_cast<char*>(sender.ipc.reply_buf);

            for(uint32_t i=0; i<copy_len; ++i) dst[i] = src[i];

        }

        

        sender.ipc.msg_len = did_copy ? copy_len : 0;

        sender.ipc.state = IpcState::Ready;

        Scheduler::instance().push_ready(sender.scheduler.id);

    }

}



} // namespace kernel

} // namespace auroraos

