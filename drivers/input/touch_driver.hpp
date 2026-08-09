// MOCK: simulator (Pure software touch simulation for QEMU, no real I2C communication)
#ifndef AURORA_TOUCH_DRIVER_HPP
#define AURORA_TOUCH_DRIVER_HPP

#include <stdint.h>
#include "device.hpp"
#include "input_event.hpp"
#include "task.hpp"

// 模拟 FT6236 手表电容触控芯片驱动
class TouchDriver : public CharDevice {
private:
    uint16_t max_x_;
    uint16_t max_y_;
    
    // QEMU 仿真环境下的内部自动拖拽状态机
    uint16_t sim_x_;
    uint16_t sim_y_;
    int      sim_step_;
    bool     sim_active_;
    int      sim_dx_;
    int      sim_dy_;

public:
    TouchDriver(const char* name, uint16_t max_x = 128, uint16_t max_y = 128)
        : CharDevice(name), max_x_(max_x), max_y_(max_y),
          sim_x_(100), sim_y_(64), sim_step_(0), sim_active_(true),
          sim_dx_(-4), sim_dy_(0) {}

    int open() override {
        // 物理硬件：向 I2C 寄存器 0x00 写入复位指令，配置中断触发引脚 (INT)
        return 0;
    }

    // ========================================================
    // 核心接口：UI 系统调用 read() 获取最新触控数据包
    // ========================================================
    int read(char* buf, int len, int offset, void* priv) override {
        if (len < static_cast<int>(sizeof(TouchPoint))) return 0;
        
        TouchPoint* point = reinterpret_cast<TouchPoint*>(buf);

        static uint32_t frame_count = 0;
        frame_count++;

        // 每隔 150 帧 (大约 5 秒) 触发一次新的模拟手势
        if (!sim_active_ && (frame_count % 150 == 0)) {
            sim_active_ = true;
            sim_step_ = 0;
            static int gesture_cycle = 0;
            gesture_cycle = (gesture_cycle + 1) % 4;
            
            if (gesture_cycle == 0 || gesture_cycle == 1) {
                // 向左滑屏：x 从 100 递减至 20
                sim_x_ = 100;
                sim_y_ = 64;
                sim_dx_ = -4;
                sim_dy_ = 0;
            } else {
                // 向右滑屏：x 从 20 递增至 100
                sim_x_ = 20;
                sim_y_ = 64;
                sim_dx_ = 4;
                sim_dy_ = 0;
            }
        }

        if (sim_active_) {
            point->x = sim_x_;
            point->y = sim_y_;
            
            if (sim_step_ == 0) {
                point->state = TouchState::PRESSED; // 手指刚接触屏幕
            } else if (sim_step_ < 20) {
                point->state = TouchState::MOVING;  // 手指正在拖拽
                sim_x_ += sim_dx_;
                sim_y_ += sim_dy_;
            } else {
                point->state = TouchState::RELEASED;// 手指离开屏幕
                sim_active_ = false;                // 结束本轮模拟
            }
            point->is_valid = true;
            sim_step_++;
        } else {
            point->is_valid = false;
        }

        return sizeof(TouchPoint);
    }
};

#endif
