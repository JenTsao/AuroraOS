#ifndef AURORA_INTENT_ENGINE_HPP
#define AURORA_INTENT_ENGINE_HPP

#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

#include "../drivers/sensor/sensor_framework.hpp"
#include "../kernel/task/app_lifecycle.hpp"

enum class ActivityClassification : uint8_t {
    Sedentary = 0,
    Walking = 1,
    Running = 2,
    HighStress = 3,
    Resting = 4
};

class IntentEngine {
public:
    struct Context {
        uint32_t last_steps = 0;
        uint32_t idle_counts = 0;
        uint16_t last_heart_rate = 75;
        uint8_t battery_level = 100;
        ActivityClassification activity = ActivityClassification::Sedentary;
        uint16_t confidence_q8 = 200; // 0-255
    };

    // 基于多传感器融合的活动推断算法
    static ActivityClassification infer_activity(uint32_t step_delta, uint16_t heart_rate, uint16_t* out_confidence = nullptr) {
        ActivityClassification result = ActivityClassification::Sedentary;
        uint16_t conf = 200;

        if (step_delta >= 30 || (step_delta > 0 && heart_rate >= 140)) {
            result = ActivityClassification::Running;
            conf = 240;
        } else if (step_delta == 0 && heart_rate >= 105) {
            // 静止状态下心率异常偏高 -> 压力或心动过速
            result = ActivityClassification::HighStress;
            conf = 230;
        } else if (step_delta > 0 || heart_rate >= 100) {
            result = ActivityClassification::Walking;
            conf = 220;
        } else if (step_delta == 0 && heart_rate < 60) {
            result = ActivityClassification::Resting;
            conf = 210;
        } else {
            result = ActivityClassification::Sedentary;
            conf = 200;
        }

        if (out_confidence) {
            *out_confidence = conf;
        }
        return result;
    }

    // 基于传感器规则与自适应意图的生命周期决策引擎
    static void process_sensors(AppControlBlock& fitness_app, Context& ctx) {
        uint32_t steps = SensorManager::instance().get_accel_sensor().get_steps();
        uint32_t delta = (steps >= ctx.last_steps) ? (steps - ctx.last_steps) : steps;

        uint16_t hr = 75;
        SensorData hr_data;
        if (SensorManager::instance().get_hr_sensor().read(&hr_data)) {
            hr = static_cast<uint16_t>(hr_data.payload.bpm);
            ctx.last_heart_rate = hr;
        }

        ctx.activity = infer_activity(delta, hr, &ctx.confidence_q8);

        // 步数或心率激增：拉起到前台
        if (delta > 0 || ctx.activity == ActivityClassification::Running || ctx.activity == ActivityClassification::Walking) {
            ctx.last_steps = steps;
            ctx.idle_counts = 0;
            if (fitness_app.state != AppState::FOREGROUND) {
                fitness_app.transition_to(AppState::FOREGROUND);

#ifdef DEBUG_INTENT
                int fd = open("/dev/uart0", O_WRONLY);
                if (fd >= 0) {
                    const char msg[] =
                        "\r\n🤖 [Intent Engine] Activity detected! Promoting Fitness App to FOREGROUND.\r\n";
                    write(fd, msg, sizeof(msg) - 1);
                    close(fd);
                }
#endif
            }
        } else {
            // 连续多次未检测到运动，平滑降级到后台 (防抖窗口)
            ctx.idle_counts++;
            if (ctx.idle_counts > 10) { // 连续无步数增加
                if (fitness_app.state == AppState::FOREGROUND) {
                    fitness_app.transition_to(AppState::BACKGROUND);
#ifdef DEBUG_INTENT
                    int fd = open("/dev/uart0", O_WRONLY);
                    if (fd >= 0) {
                        const char msg[] =
                            "\r\n🤖 [Intent Engine] Activity reduced. Demoting Fitness App to BACKGROUND.\r\n";
                        write(fd, msg, sizeof(msg) - 1);
                        close(fd);
                    }
#endif
                }
            }
        }
    }
};

#endif

