#include "firewall_service.hpp"
#include "../../net/firewall/firewall_engine.hpp"
#include "syscall.hpp"
#include <string.h>

namespace auroraos {
namespace firewall {

int g_firewall_service_ep = 4;

FirewallServer::FirewallServer() : initialized_(false) {}

void FirewallServer::init() {
    if (!initialized_) {
        ::FirewallEngine::instance().enable(true);
        initialized_ = true;
    }
}

void FirewallServer::process_request(const FirewallRequest& req, FirewallReply& reply) {
    reply.status = -1;
    ::FirewallEngine& engine = ::FirewallEngine::instance();

    switch (req.opcode) {
    case FirewallOpcode::ProcessPacket: {
        if (req.packet.len >= 0 && req.packet.len <= 1500) {
            bool accept = engine.process_packet(req.packet.payload, req.packet.len, req.packet.interface_name);
            reply.status = accept ? 0 : -1;
        }
        break;
    }
    case FirewallOpcode::AddRule:
    case FirewallOpcode::RemoveRule:
    case FirewallOpcode::GetStats:
        reply.status = 0;
        break;
    }
}

[[noreturn]] void FirewallServer::run() {
    init();
    uint32_t ep_cap = static_cast<uint32_t>(g_firewall_service_ep);

    while (true) {
        struct {
            uint32_t msg_type;
            FirewallRequest req;
        } ipc_msg;

        uint32_t caller_cap = 0;
        // sys_ipc_receive 为阻塞语义（void 返回），失败场景不存在
        sys_ipc_receive(ep_cap, &ipc_msg, sizeof(ipc_msg), &caller_cap);

        FirewallReply reply;
        reply.status = -1;

        if (ipc_msg.msg_type == 1) {
            process_request(ipc_msg.req, reply);
        }

        sys_ipc_reply(caller_cap, &reply, sizeof(reply));
    }
}

} // namespace firewall
} // namespace auroraos

extern "C" void firewall_service_entry() {
    auroraos::firewall::FirewallServer::instance().run();
}
