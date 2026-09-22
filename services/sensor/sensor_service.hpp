#ifndef SENSOR_SERVICE_HPP
#define SENSOR_SERVICE_HPP

#include "sensor_ipc.hpp"

namespace auroraos {
namespace sensor_service {

class SensorServer {
public:
    static SensorServer& instance() {
        static SensorServer server;
        return server;
    }

    void init();
    void process_request(const SensorRequest& req, SensorReply& reply);
    [[noreturn]] void run();

    // 针对测试与仿真环境的数据桩接口
    void set_mock_data(uint32_t bpm, uint32_t steps, int32_t x = 0, int32_t y = 0, int32_t z = 980);
    bool is_subscribed() const { return subscribed_; }
    uint16_t get_sample_rate() const { return sample_rate_; }

private:
    SensorServer();

    bool initialized_ = false;
    bool mock_mode_ = true;
    bool subscribed_ = false;
    uint16_t sample_rate_ = 10;

    uint32_t mock_bpm_ = 75;
    uint32_t mock_steps_ = 1200;
    int32_t mock_accel_x_ = 0;
    int32_t mock_accel_y_ = 0;
    int32_t mock_accel_z_ = 980;
};

extern int g_sensor_service_ep;

} // namespace sensor_service
} // namespace auroraos

#endif // SENSOR_SERVICE_HPP
