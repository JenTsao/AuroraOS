#ifndef AURORA_HAL_I2C_HAL_HPP
#define AURORA_HAL_I2C_HAL_HPP

#include <cstdint>
#include <cstddef>

namespace auroraos {
namespace hal {

class II2cHal {
public:
    virtual ~II2cHal() = default;

    // 向指定设备地址写入数据
    virtual bool write(uint8_t dev_addr, const uint8_t* data, size_t len) = 0;

    // 向指定设备地址寄存器写入数据
    virtual bool write_reg(uint8_t dev_addr, uint8_t reg_addr, const uint8_t* data, size_t len) = 0;

    // 从指定设备地址读取数据
    virtual bool read(uint8_t dev_addr, uint8_t* data, size_t len) = 0;

    // 从指定设备地址寄存器读取数据
    virtual bool read_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t* data, size_t len) = 0;
};

// 获取设备级 I2C 实例（由 Board 提供，根据总线 ID 区分）
II2cHal* get_i2c_hal(int bus_id);

} // namespace hal
} // namespace auroraos

#endif // AURORA_HAL_I2C_HAL_HPP
