#ifndef AURORA_HAL_GPIO_HAL_HPP
#define AURORA_HAL_GPIO_HAL_HPP

#include <stdint.h>

namespace auroraos {
namespace hal {

enum class GpioMode {
    Input,
    Output,
    Alternate,
    Analog
};

enum class GpioPull {
    None,
    PullUp,
    PullDown
};

class IGpioHal {
public:
    virtual ~IGpioHal() = default;

    // 初始化引脚模式
    virtual void init_pin(uint32_t pin, GpioMode mode, GpioPull pull = GpioPull::None) = 0;

    // 设置引脚电平
    virtual void set_pin(uint32_t pin, bool high) = 0;

    // 读取引脚电平
    virtual bool read_pin(uint32_t pin) = 0;

    // 翻转引脚电平
    virtual void toggle_pin(uint32_t pin) = 0;
};

// 获取全局或板级的 GPIO HAL 实例（由具体 Board 实现）
IGpioHal* get_gpio_hal();

} // namespace hal
} // namespace auroraos

#endif // AURORA_HAL_GPIO_HAL_HPP
