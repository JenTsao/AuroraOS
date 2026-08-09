#ifndef SOFTBUS_HPP
#define SOFTBUS_HPP

#include <stdint.h>
#include "../kernel/mutex.hpp"

// 预定义总线上最大支持的 RPC 服务数量
constexpr int MAX_RPC_HANDLERS = 5;

// RPC 回调函数签名
using RpcCallback = void (*)(const char* payload);

struct RpcNode {
    const char* cmd;
    RpcCallback handler;
};

class SerialRpcBus {
public:
    static SerialRpcBus& instance() {
        static SerialRpcBus bus;
        return bus;
    }

    void init();
    
    // 注册暴露给总线的服务
    bool register_service(const char* cmd, RpcCallback handler);
    
    // 向总线发送结构化请求
    void send_request(const char* cmd, const char* payload);
    
    // 轮询总线数据（放在后台守护线程中）
    void poll();

private:
    SerialRpcBus() = default;
    SerialRpcBus(const SerialRpcBus&) = delete;
    SerialRpcBus& operator=(const SerialRpcBus&) = delete;

    RpcNode services_[MAX_RPC_HANDLERS]{};
    int service_count_ = 0;
    Mutex tx_mutex_;

    // 状态机成员变量，取代原本的 static
    int state_ = 0;
    char cmd_buf_[16] = {0};
    char payload_buf_[64] = {0};
    int cmd_idx_ = 0;
    int pay_idx_ = 0;

    void dispatch(const char* cmd, const char* payload);
    bool strings_equal(const char* s1, const char* s2) const;
    // 返回 nullptr 表示验证失败；返回非 nullptr 表示验证通过，
    // 指针指向剥离凭证前缀（及可选分隔符）后的数据正文起始位置。
    const char* verify_auth(const char* payload) const;
};

#endif
