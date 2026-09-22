#include "power_client.hpp"
#include "syscall.hpp"
#include "../service_registry.hpp"

namespace auroraos {
namespace power_service {

PowerClient::DirectHandler PowerClient::direct_handler_ = nullptr;

PowerClient::PowerClient() : endpoint_cap_(-1) {}

void PowerClient::set_endpoint(int ep_cap) {
    endpoint_cap_ = ep_cap;
}

int PowerClient::get_endpoint() const {
    if (endpoint_cap_ >= 0) {
        return endpoint_cap_;
    }
    return services::ServiceRegistry::instance().get_service_endpoint(services::ServiceId::Power);
}

void PowerClient::set_direct_handler(DirectHandler handler) {
    direct_handler_ = handler;
}

static int do_power_call(int ep, const PowerRequest& req, PowerReply& reply, PowerClient::DirectHandler direct_handler) {
    if (direct_handler) {
        direct_handler(req, reply);
        return reply.status;
    }

    if (ep < 0) {
        ep = services::ServiceRegistry::instance().get_service_endpoint(services::ServiceId::Power);
    }

    if (ep < 0) {
        return -1;
    }

    struct {
        uint32_t msg_type;
        PowerRequest req;
    } ipc_msg;

    ipc_msg.msg_type = 1; // Power request
    ipc_msg.req = req;
    reply.status = -1;

    sys_ipc_call(static_cast<uint32_t>(ep), &ipc_msg, sizeof(ipc_msg), &reply, sizeof(reply));
    return reply.status;
}

bool PowerClient::acquire_wake_lock() {
    PowerRequest req;
    req.opcode = PowerOpcode::AcquireWakeLock;
    PowerReply reply;
    return do_power_call(get_endpoint(), req, reply, direct_handler_) == 0;
}

bool PowerClient::release_wake_lock() {
    PowerRequest req;
    req.opcode = PowerOpcode::ReleaseWakeLock;
    PowerReply reply;
    return do_power_call(get_endpoint(), req, reply, direct_handler_) == 0;
}

uint8_t PowerClient::get_battery_level() {
    PowerRequest req;
    req.opcode = PowerOpcode::GetBatteryLevel;
    PowerReply reply;
    if (do_power_call(get_endpoint(), req, reply, direct_handler_) == 0) {
        return reply.data.battery_percent;
    }
    return 0;
}

PowerState PowerClient::get_power_state() {
    PowerRequest req;
    req.opcode = PowerOpcode::GetPowerState;
    PowerReply reply;
    if (do_power_call(get_endpoint(), req, reply, direct_handler_) == 0) {
        return reply.data.current_state;
    }
    return PowerState::RUN;
}

} // namespace power_service
} // namespace auroraos
