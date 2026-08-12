#include "../../syscall/syscall.hpp"
#include "firewall_client.hpp"
#include "firewall_ipc.hpp"
#include "../../kernel/core/syscall_ipc.hpp"
#include <cstring>

namespace auroraos {
namespace firewall {

void FirewallClient::init() {
    service_ep_cap_ = g_firewall_service_ep;
}

void FirewallClient::set_endpoint(int ep_cap) {
    service_ep_cap_ = ep_cap;
}

bool FirewallClient::process_packet(const uint8_t* packet, int len, const char* interface) {
    if (service_ep_cap_ < 0) {
        service_ep_cap_ = g_firewall_service_ep;
    }

    if (service_ep_cap_ < 0) {
        // 如果没有防火墙服务，默认放行
        return true;
    }
    
    if (len > 1500) {
        return false;
    }

    struct {
        uint32_t msg_type;
        FirewallRequest req;
    } ipc_msg;

    ipc_msg.msg_type = 1;
    ipc_msg.req.opcode = FirewallOpcode::ProcessPacket;
    ipc_msg.req.packet.len = len;
    std::strncpy(ipc_msg.req.packet.interface_name, interface, sizeof(ipc_msg.req.packet.interface_name) - 1);
    ipc_msg.req.packet.interface_name[sizeof(ipc_msg.req.packet.interface_name) - 1] = '\0';
    std::memcpy(ipc_msg.req.packet.payload, packet, len);

    FirewallReply reply;
    reply.status = -1;

    sys_ipc_call(static_cast<uint32_t>(service_ep_cap_), &ipc_msg, sizeof(ipc_msg), &reply, sizeof(reply));

    return reply.status == 0;
}

} // namespace firewall
} // namespace auroraos



