#include "power_service.hpp"
#include "syscall.hpp"

namespace auroraos {
namespace power_service {

int g_power_service_ep = 7; // Default placeholder capability ID

PowerServer::PowerServer() : initialized_(false) {}

void PowerServer::init() {
    if (!initialized_) {
        PowerManager::instance().reset();
        initialized_ = true;
    }
}

void PowerServer::process_request(const PowerRequest& req, PowerReply& reply, uint32_t caller_id) {
    reply.status = -1;

    switch (req.opcode) {
    case PowerOpcode::AcquireWakeLock:
        PowerManager::instance().acquire_wake_lock(caller_id);
        reply.status = 0;
        break;
    case PowerOpcode::ReleaseWakeLock:
        PowerManager::instance().release_wake_lock(caller_id);
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

[[noreturn]] void PowerServer::run() {
    init();
    uint32_t ep_cap = static_cast<uint32_t>(g_power_service_ep);

    while (true) {
        struct {
            uint32_t msg_type;
            PowerRequest req;
        } ipc_msg;

        uint32_t caller_cap = 0;
        // sys_ipc_receive 为阻塞语义（void 返回），失败场景不存在
        sys_ipc_receive(ep_cap, &ipc_msg, sizeof(ipc_msg), &caller_cap);

        PowerReply reply;
        reply.status = -1;

        if (ipc_msg.msg_type == 1) {
            process_request(ipc_msg.req, reply, caller_cap);
        }

        sys_ipc_reply(caller_cap, &reply, sizeof(reply));
    }
}

} // namespace power_service
} // namespace auroraos

extern "C" void power_service_entry() {
    auroraos::power_service::PowerServer::instance().run();
}
