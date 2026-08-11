#ifndef IPC_HPP
#define IPC_HPP

#include <stdint.h>

struct TaskControlBlock;

namespace auroraos {
namespace kernel {

enum class IpcState : uint8_t {
    Ready = 0,
    Receiving = 1,
    ReplyBlocked = 2,
    Sending = 3
};

// ============================================================
// Typed IPC Message System
// ============================================================

// IPC message type IDs — 0 is reserved for raw/untyped messages
enum class IpcMsgType : uint32_t {
    Raw = 0,        // Untyped raw message (backward compatible)
    UserBase = 64,  // User-defined message types start here
};

// Message header prepended to every typed message
struct IpcMsgHeader {
    uint32_t msg_type;     // IpcMsgType or user-defined ID
    uint32_t payload_size; // Size of payload in bytes
};

// Raw message header (type=0, backward compatible)
struct IpcRawMessage {
    IpcMsgType msg_type = IpcMsgType::Raw;
    uint32_t payload_size;
    // Followed by raw bytes
};

// Typed IPC message wrapper
template<typename T>
struct IpcMessage {
    static_assert(sizeof(T) > 0, "IPC message payload must not be empty");
    static_assert(sizeof(T) <= 4088, "IPC message payload exceeds 4KB limit");

    IpcMsgType msg_type;
    uint32_t payload_size;
    T payload;

    // Total wire size including header
    static constexpr uint32_t wire_size() {
        return sizeof(IpcMessage<T>);
    }

    // Create from type and payload
    static IpcMessage<T> create(IpcMsgType type, const T& data) {
        IpcMessage<T> msg;
        msg.msg_type = type;
        msg.payload_size = sizeof(T);
        msg.payload = data;
        return msg;
    }
};

// Runtime type validation helper
inline bool ipc_validate_type(const void* msg, uint32_t len, uint32_t expected_type) {
    if (!msg || len < sizeof(IpcRawMessage)) return false;
    const auto* hdr = static_cast<const IpcRawMessage*>(msg);
    return hdr->msg_type == static_cast<IpcMsgType>(expected_type);
}

// ============================================================
// Endpoint Class
// ============================================================

class Endpoint {
public:
    Endpoint() : send_head_(nullptr), send_tail_(nullptr), recv_head_(nullptr), recv_tail_(nullptr) {}

    // Synchronous IPC Call
    void call(TaskControlBlock* sender, void* msg, uint32_t len, void* reply_buf, uint32_t max_reply_len);

    // Synchronous IPC Receive
    void receive(TaskControlBlock* receiver, void* msg_buf, uint32_t max_len);

    // Synchronous IPC Reply
    static void reply(TaskControlBlock* receiver, uint32_t sender_id, void* reply_msg, uint32_t len);

private:
    void enqueue_sender(TaskControlBlock* tcb);
    TaskControlBlock* dequeue_sender();
    
    void enqueue_receiver(TaskControlBlock* tcb);
    TaskControlBlock* dequeue_receiver();

    TaskControlBlock* send_head_;
    TaskControlBlock* send_tail_;
    
    TaskControlBlock* recv_head_;
    TaskControlBlock* recv_tail_;
};

// ============================================================
// Type-Safe IPC Helpers
// ============================================================

// Type-safe IPC call — compile-time checked
template<typename T>
void ipc_call(Endpoint& ep, TaskControlBlock* sender,
              IpcMsgType type, const T& payload,
              void* reply_buf, uint32_t max_reply_len) {
    auto msg = IpcMessage<T>::create(type, payload);
    ep.call(sender, &msg, sizeof(msg), reply_buf, max_reply_len);
}

// Type-safe IPC receive — returns true if type matches
template<typename T>
bool ipc_receive(Endpoint& ep, TaskControlBlock* receiver,
                 IpcMsgType expected_type, T& out_payload) {
    IpcMessage<T> msg;
    char recv_buf[sizeof(IpcMessage<T>)];
    ep.receive(receiver, recv_buf, sizeof(recv_buf));

    const auto* typed = reinterpret_cast<const IpcMessage<T>*>(recv_buf);
    if (typed->msg_type != expected_type) return false;
    out_payload = typed->payload;
    return true;
}

} // namespace kernel
} // namespace auroraos

#endif // IPC_HPP
