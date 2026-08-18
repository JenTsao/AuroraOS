// =============================================================================
// drivers/input/touch_driver.hpp
//
// 触控屏通用字符设备驱动 (TouchDriver)
// 统一封装汇顶 GT316 真实 I2C 硬件通信与 QEMU 仿真回退
// =============================================================================
#ifndef AURORA_TOUCH_DRIVER_HPP
#define AURORA_TOUCH_DRIVER_HPP

#include <stdint.h>
#include "../../kernel/core/device.hpp"
#include "input_event.hpp"
#include "gt316_driver.hpp"

// 兼容别名：TouchDriver 继承 Gt316Driver，具备完整的真实硬件与仿真双模能力
class TouchDriver : public Gt316Driver {
public:
    explicit TouchDriver(const char* name = "touch0",
                         uint16_t max_x = Gt316Driver::kDefaultWidth,
                         uint16_t max_y = Gt316Driver::kDefaultHeight)
        : Gt316Driver(name, max_x, max_y) {}
};

#endif // AURORA_TOUCH_DRIVER_HPP
