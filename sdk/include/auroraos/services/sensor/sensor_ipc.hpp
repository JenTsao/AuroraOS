#ifndef AURORAOS_SENSOR_IPC_HPP
#define AURORAOS_SENSOR_IPC_HPP

#include <stdint.h>

namespace auroraos {
namespace sensor_service {

enum class SensorOpcode : uint32_t {
    Subscribe = 1,
    Unsubscribe = 2,
    SetSampleRate = 3,
    ReadLatest = 4
};

// 重用驱动层的 SensorType，这里简单定义以解耦
enum class IpcSensorType : uint8_t {
    HEART_RATE = 0,
    ACCELEROMETER = 1,
    STEP_COUNTER = 2
};

struct SubscribeReq {
    IpcSensorType type;
};

struct SetSampleRateReq {
    IpcSensorType type;
    uint16_t rate_hz;
};

struct ReadLatestReq {
    IpcSensorType type;
};

struct SensorRequest {
    SensorOpcode opcode;

    union {
        SubscribeReq subscribe;
        SetSampleRateReq set_rate;
        ReadLatestReq read_latest;
    };
};

struct SensorReply {
    int status; // 0 for success, negative for error

    // For ReadLatest
    uint32_t timestamp;

    union {
        struct {
            int32_t x;
            int32_t y;
            int32_t z;
        } accel;

        uint32_t bpm;
        uint32_t steps;
    } data;
};

} // namespace sensor_service
} // namespace auroraos

#endif // AURORAOS_SENSOR_IPC_HPP
