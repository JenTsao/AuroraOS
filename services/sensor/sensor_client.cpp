#include "sensor_client.hpp"
#include "syscall.hpp"
#include "../service_registry.hpp"

namespace auroraos {
namespace sensor_service {

SensorClient::DirectHandler SensorClient::direct_handler_ = nullptr;

SensorClient::SensorClient() : endpoint_cap_(-1) {}

void SensorClient::set_endpoint(int ep_cap) {
    endpoint_cap_ = ep_cap;
}

int SensorClient::get_endpoint() const {
    if (endpoint_cap_ >= 0) {
        return endpoint_cap_;
    }
    return services::ServiceRegistry::instance().get_service_endpoint(services::ServiceId::Sensor);
}

void SensorClient::set_direct_handler(DirectHandler handler) {
    direct_handler_ = handler;
}

static int do_sensor_call(int ep, const SensorRequest& req, SensorReply& reply, SensorClient::DirectHandler direct_handler) {
    if (direct_handler) {
        direct_handler(req, reply);
        return reply.status;
    }

    if (ep < 0) {
        ep = services::ServiceRegistry::instance().get_service_endpoint(services::ServiceId::Sensor);
    }

    if (ep < 0) {
        return -1;
    }

    struct {
        uint32_t msg_type;
        SensorRequest req;
    } ipc_msg;

    ipc_msg.msg_type = 1; // Sensor request
    ipc_msg.req = req;
    reply.status = -1;

    sys_ipc_call(static_cast<uint32_t>(ep), &ipc_msg, sizeof(ipc_msg), &reply, sizeof(reply));
    return reply.status;
}

bool SensorClient::subscribe(IpcSensorType type) {
    SensorRequest req;
    req.opcode = SensorOpcode::Subscribe;
    req.subscribe.type = type;

    SensorReply reply;
    return do_sensor_call(get_endpoint(), req, reply, direct_handler_) == 0;
}

bool SensorClient::unsubscribe(IpcSensorType type) {
    SensorRequest req;
    req.opcode = SensorOpcode::Unsubscribe;
    req.subscribe.type = type;

    SensorReply reply;
    return do_sensor_call(get_endpoint(), req, reply, direct_handler_) == 0;
}

bool SensorClient::set_sample_rate(IpcSensorType type, uint16_t rate_hz) {
    SensorRequest req;
    req.opcode = SensorOpcode::SetSampleRate;
    req.set_rate.type = type;
    req.set_rate.rate_hz = rate_hz;

    SensorReply reply;
    return do_sensor_call(get_endpoint(), req, reply, direct_handler_) == 0;
}

bool SensorClient::read_latest(IpcSensorType type, SensorReply& reply) {
    SensorRequest req;
    req.opcode = SensorOpcode::ReadLatest;
    req.read_latest.type = type;

    return do_sensor_call(get_endpoint(), req, reply, direct_handler_) == 0;
}

uint32_t SensorClient::read_heart_rate() {
    SensorReply reply;
    if (read_latest(IpcSensorType::HEART_RATE, reply)) {
        return reply.data.bpm;
    }
    return 0;
}

uint32_t SensorClient::read_steps() {
    SensorReply reply;
    if (read_latest(IpcSensorType::STEP_COUNTER, reply)) {
        return reply.data.steps;
    }
    return 0;
}

} // namespace sensor_service
} // namespace auroraos
