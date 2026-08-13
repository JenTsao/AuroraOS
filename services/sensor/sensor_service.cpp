#include "sensor_ipc.hpp"
#include "syscall.hpp"
#include "../../drivers/sensor/sensor_framework.hpp"

extern "C" {
void sensor_service_entry();
}

namespace auroraos {
namespace sensor_service {

int g_sensor_service_ep = 6; // Placeholder capability ID

} // namespace sensor_service
} // namespace auroraos

extern "C" void sensor_service_entry() {
    using namespace auroraos::sensor_service;

    // Initialize Hardware drivers and Manager
    // Using the original SensorManager as our underlying driver proxy
    SensorManager& sm = SensorManager::instance();
    sm.init_all();

    uint32_t ep_cap = g_sensor_service_ep;

    while (true) {
        struct {
            uint32_t msg_type;
            SensorRequest req;
        } ipc_msg;

        uint32_t caller_cap = 0;
        sys_ipc_receive(ep_cap, &ipc_msg, sizeof(ipc_msg), &caller_cap);

        SensorReply reply;
        reply.status = -1;

        if (ipc_msg.msg_type == 1) { // Sensor Request
            switch (ipc_msg.req.opcode) {
            case SensorOpcode::Subscribe:
            case SensorOpcode::Unsubscribe: {
                // Access Control Check:
                // Verify if caller_cap has Sensor capabilities using AppSandbox?
                // For now, accept blindly.
                reply.status = 0;
                break;
            }
            case SensorOpcode::SetSampleRate: {
                reply.status = 0;
                break;
            }
            case SensorOpcode::ReadLatest: {
                SensorData out_data;
                bool success = false;

                if (ipc_msg.req.read_latest.type == IpcSensorType::HEART_RATE) {
                    // Assuming read_hr is available or we read from a buffer
                    // But wait, the manager processes it asynchronously.
                    // We can just query health engine for latest.
                    reply.data.bpm = sm.get_health_engine().get_filtered_bpm();
                    success = true;
                } else if (ipc_msg.req.read_latest.type == IpcSensorType::STEP_COUNTER) {
                    reply.data.steps = sm.get_health_engine().get_total_steps();
                    success = true;
                }

                if (success) {
                    reply.status = 0;
                    reply.timestamp = 0; // Replace with sys_get_time()
                } else {
                    reply.status = -1;
                }
                break;
            }
            }
        }

        sys_ipc_reply(caller_cap, &reply, sizeof(reply));
    }
}
