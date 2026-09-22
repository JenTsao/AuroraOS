#include "sensor_service.hpp"
#include "syscall.hpp"
#include "../../drivers/sensor/sensor_framework.hpp"

namespace auroraos {
namespace sensor_service {

int g_sensor_service_ep = 6; // Default placeholder capability ID

SensorServer::SensorServer() : initialized_(false), mock_mode_(true), subscribed_(false), sample_rate_(10) {}

void SensorServer::init() {
    if (!initialized_) {
        // Under host test or simulation, keep mock mode active unless hardware requested
        initialized_ = true;
    }
}

void SensorServer::set_mock_data(uint32_t bpm, uint32_t steps, int32_t x, int32_t y, int32_t z) {
    mock_bpm_ = bpm;
    mock_steps_ = steps;
    mock_accel_x_ = x;
    mock_accel_y_ = y;
    mock_accel_z_ = z;
    mock_mode_ = true;
}

void SensorServer::process_request(const SensorRequest& req, SensorReply& reply) {
    reply.status = -1;
    reply.timestamp = sys_get_time();

    switch (req.opcode) {
    case SensorOpcode::Subscribe:
        subscribed_ = true;
        reply.status = 0;
        break;
    case SensorOpcode::Unsubscribe:
        subscribed_ = false;
        reply.status = 0;
        break;
    case SensorOpcode::SetSampleRate:
        sample_rate_ = req.set_rate.rate_hz;
        reply.status = 0;
        break;
    case SensorOpcode::ReadLatest:
        if (req.read_latest.type == IpcSensorType::HEART_RATE) {
            reply.data.bpm = mock_bpm_;
            reply.status = 0;
        } else if (req.read_latest.type == IpcSensorType::STEP_COUNTER) {
            reply.data.steps = mock_steps_;
            reply.status = 0;
        } else if (req.read_latest.type == IpcSensorType::ACCELEROMETER) {
            reply.data.accel.x = mock_accel_x_;
            reply.data.accel.y = mock_accel_y_;
            reply.data.accel.z = mock_accel_z_;
            reply.status = 0;
        }
        break;
    }
}

[[noreturn]] void SensorServer::run() {
    init();
    uint32_t ep_cap = static_cast<uint32_t>(g_sensor_service_ep);

    while (true) {
        struct {
            uint32_t msg_type;
            SensorRequest req;
        } ipc_msg;

        uint32_t caller_cap = 0;
        sys_ipc_receive(ep_cap, &ipc_msg, sizeof(ipc_msg), &caller_cap);

        SensorReply reply;
        reply.status = -1;

        if (ipc_msg.msg_type == 1) {
            process_request(ipc_msg.req, reply);
        }

        sys_ipc_reply(caller_cap, &reply, sizeof(reply));
    }
}

} // namespace sensor_service
} // namespace auroraos

extern "C" void sensor_service_entry() {
    auroraos::sensor_service::SensorServer::instance().run();
}
