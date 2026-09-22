#ifndef AURORAOS_SENSOR_CLIENT_HPP
#define AURORAOS_SENSOR_CLIENT_HPP

#include "sensor_ipc.hpp"
#include <stdint.h>

namespace auroraos {
namespace sensor_service {

class SensorClient {
public:
    static SensorClient& instance() {
        static SensorClient client;
        return client;
    }

    void set_endpoint(int ep_cap);
    int get_endpoint() const;

    bool subscribe(IpcSensorType type);
    bool unsubscribe(IpcSensorType type);
    bool set_sample_rate(IpcSensorType type, uint16_t rate_hz);
    bool read_latest(IpcSensorType type, SensorReply& reply);

    // 高层便捷函数
    uint32_t read_heart_rate();
    uint32_t read_steps();

    // 直接分发器（用于单元测试和无 IPC 调度环境）
    using DirectHandler = void (*)(const SensorRequest& req, SensorReply& reply);
    static void set_direct_handler(DirectHandler handler);

private:
    SensorClient();
    int endpoint_cap_ = -1;
    static DirectHandler direct_handler_;
};

} // namespace sensor_service
} // namespace auroraos

#endif // AURORAOS_SENSOR_CLIENT_HPP
