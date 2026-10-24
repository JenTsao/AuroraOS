#ifndef AURORAOS_FIREWALL_IPC_HPP
#define AURORAOS_FIREWALL_IPC_HPP

#include <stdint.h>

namespace auroraos {
namespace firewall {

enum class FirewallOpcode : uint32_t {
    ProcessPacket = 1,
    AddRule = 2,
    RemoveRule = 3,
    GetStats = 4
};

struct ProcessPacketReq {
    int len;
    char interface_name[8];
    uint8_t payload[1500]; // Max standard MTU
};

struct RuleReq {
    char rule_str[64];
};

struct FirewallRequest {
    FirewallOpcode opcode;

    union {
        ProcessPacketReq packet;
        RuleReq rule;
    };
};

struct FirewallReply {
    int status; // 0 for accept/success, <0 for drop/error
};

// 全局服务端点引用（供测试和直接调用时使用）
extern int g_firewall_service_ep;

} // namespace firewall
} // namespace auroraos

#endif // AURORAOS_FIREWALL_IPC_HPP
