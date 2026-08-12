#ifndef AURORAOS_RUNTIME_IPC_HPP
#define AURORAOS_RUNTIME_IPC_HPP

#include "syscall.hpp"
#include <stdint.h>

namespace auroraos {
namespace runtime {

// 高阶 IPC 包装，对应用层隐藏系统调用的细节
class IpcChannel {
public:
    IpcChannel(uint32_t target_ep) : target_ep_(target_ep) {}

    // 发送带有固定负载的同步请求
    template <typename ReqT, typename ResT>
    bool call(uint32_t msg_type, const ReqT& req, ResT& res) {
        struct {
            uint32_t type;
            ReqT payload;
        } req_msg;
        req_msg.type = msg_type;
        req_msg.payload = req;

        sys_ipc_call(target_ep_, &req_msg, sizeof(req_msg), &res, sizeof(res));
        return true; 
    }

private:
    uint32_t target_ep_;
};

class IpcServer {
public:
    IpcServer(uint32_t ep) : ep_(ep) {}

    // 阻塞接收消息
    template <typename MsgT>
    bool receive(MsgT& msg, uint32_t& sender_id) {
        sys_ipc_receive(ep_, &msg, sizeof(msg), &sender_id);
        return true;
    }

    // 回复消息
    template <typename ResT>
    void reply(uint32_t sender_id, const ResT& res) {
        sys_ipc_reply(sender_id, const_cast<ResT*>(&res), sizeof(ResT));
    }

private:
    uint32_t ep_;
};

} // namespace runtime
} // namespace auroraos

#endif // AURORAOS_RUNTIME_IPC_HPP
