// =============================================================================
// kernel/core/ipc.hpp
//
// 统一进程间通信 (IPC) 端点与类型化消息子系统
// 核心特性：
//   1. 同步阻塞、非阻塞 (nb_call / nb_receive) 与超时截止时间 (Timeout / Deadline)
//   2. 端点单向/双向消息传递模型 (Endpoint Call-Receive-Reply)
//   3. 编译期类型安全与运行时安全校验 (Typed IPC Message System)
//   4. 严格 4KB 消息硬上限与零动态内存分配防御 (DoS Prevention)
// =============================================================================
#ifndef IPC_HPP
#define IPC_HPP

#include <stdint.h>
#include "../task/wait_queue.hpp"
#include "kernel_object.hpp"

struct TaskControlBlock;

namespace auroraos {
namespace kernel {

enum class IpcState : uint8_t {
    Ready = 0,
    Receiving = 1,
    ReplyBlocked = 2,
    Sending = 3
};

// IPC 超时与非阻塞控制常量
constexpr uint32_t IPC_TIMEOUT_INFINITE = 0xFFFFFFFFU; // 无限等待 (阻塞)
constexpr uint32_t IPC_NONBLOCK         = 0x00000000U; // 非阻塞 (立刻返回)

// IPC 操作状态码
enum class IpcStatus : int32_t {
    Ok = 0,             // 成功
    Timeout = -1,       // 操作超时 (ETIMEDOUT / -110)
    WouldBlock = -2,    // 非阻塞且对端未就绪 (EWOULDBLOCK / EAGAIN / -11)
    Invalid = -3,       // 参数无效 / 空指针 (EINVAL / -22)
    NoPermission = -4,  // 权能不足 (EACCES / -13)
    ReceiverDead = -5   // 接收方或回复目标不存在 (ESRCH / -3)
};

// ============================================================
// Typed IPC Message System
// ============================================================

// IPC 消息类型枚举 (0 预留为原始/未类型化消息)
enum class IpcMsgType : uint32_t {
    Raw = 0,       // 原始二进制消息 (向后兼容)
    UserBase = 64, // 用户自定义类型基准
};

// 类型化消息头部
struct IpcMsgHeader {
    uint32_t msg_type;     // IpcMsgType 或用户定义 ID
    uint32_t payload_size; // 载荷字节长度
};

// 原始二进制消息头部
struct IpcRawMessage {
    IpcMsgType msg_type = IpcMsgType::Raw;
    uint32_t payload_size;
};

// 类型安全消息包装模板
template <typename T> struct IpcMessage {
    static_assert(sizeof(T) > 0, "IPC message payload must not be empty");
    static_assert(sizeof(T) <= 4088, "IPC message payload exceeds 4KB limit");

    IpcMsgType msg_type;
    uint32_t payload_size;
    T payload;

    static constexpr uint32_t wire_size() {
        return sizeof(IpcMessage<T>);
    }

    static IpcMessage<T> create(IpcMsgType type, const T& data) {
        IpcMessage<T> msg;
        msg.msg_type = type;
        msg.payload_size = sizeof(T);
        msg.payload = data;
        return msg;
    }
};

// 运行时类型校验辅助函数
inline bool ipc_validate_type(const void* msg, uint32_t len, uint32_t expected_type) {
    if (!msg || len < sizeof(IpcRawMessage))
        return false;

    const auto* hdr = static_cast<const IpcRawMessage*>(msg);
    return hdr->msg_type == static_cast<IpcMsgType>(expected_type);
}

// ============================================================
// Endpoint 类
// ============================================================

class Endpoint : public KernelObject {
public:
    Endpoint() : KernelObject(ObjectType::Endpoint) {}

    // IPC Call: 发送请求并等待对端回复
    // 支持超时参数 timeout_ticks:
    //   - IPC_TIMEOUT_INFINITE (默认): 阻塞直到对端应答
    //   - IPC_NONBLOCK (0): 非阻塞调用 (nb_call)。若无等待中的 receiver 则立即返回 WouldBlock
    //   - > 0: 设置超时上限 (Tick 计数)，超时未得到处理自动取消并唤醒返回 Timeout
    IpcStatus call(TaskControlBlock* sender, void* msg, uint32_t len,
                   void* reply_buf, uint32_t max_reply_len,
                   uint32_t timeout_ticks = IPC_TIMEOUT_INFINITE);

    // 非阻塞 IPC Call 便捷包装 (nb_call)
    IpcStatus nb_call(TaskControlBlock* sender, void* msg, uint32_t len,
                      void* reply_buf, uint32_t max_reply_len) {
        return call(sender, msg, len, reply_buf, max_reply_len, IPC_NONBLOCK);
    }

    // IPC Receive: 接收对端发送的消息
    // 支持超时参数 timeout_ticks:
    //   - IPC_TIMEOUT_INFINITE (默认): 阻塞直到有发送方消息到达
    //   - IPC_NONBLOCK (0): 非阻塞接收 (nb_receive)。若无等待的 sender 则立即返回 WouldBlock
    //   - > 0: 设置超时上限 (Tick 计数)
    IpcStatus receive(TaskControlBlock* receiver, void* msg_buf, uint32_t max_len,
                      uint32_t timeout_ticks = IPC_TIMEOUT_INFINITE);

    // 非阻塞 IPC Receive 便捷包装 (nb_receive)
    IpcStatus nb_receive(TaskControlBlock* receiver, void* msg_buf, uint32_t max_len) {
        return receive(receiver, msg_buf, max_len, IPC_NONBLOCK);
    }

    // IPC Reply: 回复已阻塞等待应答的发送方 (非阻塞，恢复发送方执行)
    static IpcStatus reply(TaskControlBlock* receiver, uint32_t sender_id, void* reply_msg, uint32_t len);

    // 超时取消接口 (由调度器超时机制或任务异常终止时调用)
    void cancel_waiter(TaskControlBlock* task);

private:
    WaitQueue send_queue_;
    WaitQueue recv_queue_;
};

// ============================================================
// 类型安全 IPC 模板辅助函数
// ============================================================

template <typename T>
IpcStatus ipc_call(Endpoint& ep, TaskControlBlock* sender, IpcMsgType type, const T& payload,
                   void* reply_buf, uint32_t max_reply_len,
                   uint32_t timeout_ticks = IPC_TIMEOUT_INFINITE) {
    auto msg = IpcMessage<T>::create(type, payload);
    return ep.call(sender, &msg, sizeof(msg), reply_buf, max_reply_len, timeout_ticks);
}

template <typename T>
IpcStatus ipc_receive(Endpoint& ep, TaskControlBlock* receiver, IpcMsgType expected_type,
                      T& out_payload, uint32_t timeout_ticks = IPC_TIMEOUT_INFINITE) {
    char recv_buf[sizeof(IpcMessage<T>)];
    IpcStatus st = ep.receive(receiver, recv_buf, sizeof(recv_buf), timeout_ticks);
    if (st != IpcStatus::Ok)
        return st;

    const auto* typed = reinterpret_cast<const IpcMessage<T>*>(recv_buf);
    if (typed->msg_type != expected_type)
        return IpcStatus::Invalid;

    out_payload = typed->payload;
    return IpcStatus::Ok;
}

} // namespace kernel
} // namespace auroraos

#endif // IPC_HPP
