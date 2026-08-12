#include "firewall_ipc.hpp"
#include "../../net/firewall/firewall_engine.hpp"
#include "syscall.hpp"
#include <cstring>

using namespace auroraos::firewall;

namespace auroraos {
namespace firewall {
int g_firewall_service_ep = 4;
}
}

extern "C" void firewall_service_entry() {
    ::FirewallEngine& engine = ::FirewallEngine::instance();
    engine.enable(true);

    uint32_t ep_cap = auroraos::firewall::g_firewall_service_ep;

    while (true) {
        struct {
            uint32_t msg_type;
            FirewallRequest req;
        } ipc_msg;
        
        uint32_t caller_cap = 0;
        sys_ipc_receive(ep_cap, &ipc_msg, sizeof(ipc_msg), &caller_cap);
        
        FirewallReply reply;
        reply.status = -1;

        if (ipc_msg.msg_type == 1) { // Firewall Request
            switch (ipc_msg.req.opcode) {
                case FirewallOpcode::ProcessPacket: {
                    if (ipc_msg.req.packet.len >= 0 && ipc_msg.req.packet.len <= 1500) {
                        bool accept = engine.process_packet(
                            ipc_msg.req.packet.payload,
                            ipc_msg.req.packet.len,
                            ipc_msg.req.packet.interface_name
                        );
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
        
        sys_ipc_reply(caller_cap, &reply, sizeof(reply));
    }
}

