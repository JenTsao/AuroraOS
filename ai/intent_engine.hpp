#ifndef AURORA_INTENT_ENGINE_HPP
#define AURORA_INTENT_ENGINE_HPP

#include <fcntl.h>
#include <unistd.h>

#include "../drivers/sensor/sensor_framework.hpp"
#include "../kernel/app_lifecycle.hpp"

class IntentEngine {
public:
    struct Context {
        uint32_t last_steps = 0;
        uint32_t idle_counts = 0;
    };

    // 基于传感器规则的决策引擎
    static void process_sensors(AppControlBlock& fitness_app, Context& ctx) {
        uint32_t steps = SensorManager::instance().get_accel_sensor().get_steps();

        // 简易意图逻辑：滑动窗口防抖。
        // 使用 != 而不是 >，安全处理达到 UINT32_MAX 后的整数回绕 (wraparound)
        if (steps != ctx.last_steps) { 
            ctx.last_steps = steps;
            ctx.idle_counts = 0;
            if (fitness_app.state != AppState::FOREGROUND) {
                fitness_app.transition_to(AppState::FOREGROUND);
                
#ifdef DEBUG_INTENT
                int fd = open("/dev/uart0", O_WRONLY);
                if (fd >= 0) {
                    const char msg[] = "\r\n🤖 [Intent Engine] High activity detected! Promoting Fitness App to FOREGROUND.\r\n";
                    write(fd, msg, sizeof(msg) - 1);
                    close(fd);
                }
#endif
            }
        } else {
            // 否则如果连续多次未检测到运动，降级到后台 (防抖)
            ctx.idle_counts++;
            if (ctx.idle_counts > 10) { // 连续 ~5秒无步数增加
                if (fitness_app.state == AppState::FOREGROUND) {
                    fitness_app.transition_to(AppState::BACKGROUND);
#ifdef DEBUG_INTENT
                    int fd = open("/dev/uart0", O_WRONLY);
                    if (fd >= 0) {
                        const char msg[] = "\r\n🤖 [Intent Engine] Activity reduced. Demoting Fitness App to BACKGROUND.\r\n";
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
