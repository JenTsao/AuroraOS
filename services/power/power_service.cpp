#include "power_ipc.hpp"
#include "power_manager.hpp"
#include "syscall.hpp"

extern "C" {
void power_service_entry();
}

namespace auroraos {
namespace power_service {

int g_power_service_ep = 7; // Placeholder capability ID

} // namespace power_service
} // namespace auroraos

extern "C" void power_service_entry() {
    using namespace auroraos::power_service;

    uint32_t ep_cap = g_power_service_ep;

    while (true) {
        struct {
            uint32_t msg_type;
            PowerRequest req;
        } ipc_msg;

        uint32_t caller_cap = 0;
        sys_ipc_receive(ep_cap, &ipc_msg, sizeof(ipc_msg), &caller_cap);

        PowerReply reply;
        reply.status = -1;

        if (ipc_msg.msg_type == 1) { // Power Request
            switch (ipc_msg.req.opcode) {
            case PowerOpcode::AcquireWakeLock:
                PowerManager::instance().acquire_wake_lock(caller_cap);
                reply.status = 0;
                break;
            case PowerOpcode::ReleaseWakeLock:
                PowerManager::instance().release_wake_lock(caller_cap);
                reply.status = 0;
                break;
            case PowerOpcode::GetBatteryLevel:
                reply.data.battery_percent = PowerManager::instance().get_battery_level();
                reply.status = 0;
                break;
            case PowerOpcode::GetPowerState:
                reply.data.current_state = PowerManager::instance().get_current_state();
                reply.status = 0;
                break;
            }
        }

        sys_ipc_reply(caller_cap, &reply, sizeof(reply));
    }
}
