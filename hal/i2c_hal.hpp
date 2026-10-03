#ifndef AURORA_HAL_I2C_HAL_HPP
#define AURORA_HAL_I2C_HAL_HPP

#include <stdint.h>
#include <stddef.h>

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
    //
    // 【原子性】本接口的语义是「单次总线事务内完成写寄存器地址 + 读回数据」。
    // 实现方必须用单次锁覆盖「写 reg_hi/reg_lo + CMD 触发 + 等完成 +
    // FIFO 读出」全程，否则两次加锁之间的窗口会让同总线的其他客户端
    // （触摸与加速度计共用 SENSOR_I2C_PORT）插入事务，读到撕裂数据。
    virtual bool read_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t* data, size_t len) = 0;

    // 【16 位寄存器地址版本】某些器件（如 GT316）的寄存器地址是 16 位
    // （如 0x814E），无法用上面的 uint8_t 版本表达——直接调用会把
    // 0x814E 截断成 0x4E，静默读到错误寄存器。
    //
    // 原子性要求与 read_reg 完全相同：单次锁覆盖「写 reg_hi/reg_lo +
    // CMD 触发 + 等完成 + FIFO 读出」全程。
    //
    // 为什么不是纯虚函数：8 位寄存器器件（SSD1306 / BHY2 / OV2640）不需要
    // 本接口，强制它们实现只会制造无意义的桩。默认实现明确返回 false
    // 而不是退化成 write()+read()——那正是本接口要消除的非原子写法，
    // 静默退化会让调用方以为已受保护。
    virtual bool read_reg16(uint8_t dev_addr, uint16_t reg_addr, uint8_t* data, size_t len) {
        (void)dev_addr;
        (void)reg_addr;
        (void)data;
        (void)len;
        return false;
    }

    // 【16 位寄存器地址写入】原子性要求与 read_reg16 相同：单次锁覆盖
    // 「写 reg_hi/reg_lo + data + CMD 触发 + 等完成」全程。
    //
    // 注意 GT316 的写方向本身把 [reg_hi, reg_lo, data...] 打包成单次
    // write() 调用（见 Gt316Driver::write_reg16），write() 内部已是
    // 单次持锁，故当前调用方无需迁移到本接口。保留此重载是为了让
    // 「16 位寄存器」这一类访问有一致的原子入口，而非让调用方各自拼装。
    virtual bool write_reg16(uint8_t dev_addr, uint16_t reg_addr, const uint8_t* data, size_t len) {
        (void)dev_addr;
        (void)reg_addr;
        (void)data;
        (void)len;
        return false;
    }
};

// 获取设备级 I2C 实例（由 Board 提供，根据总线 ID 区分）
II2cHal* get_i2c_hal(int bus_id);

} // namespace hal
} // namespace auroraos

#endif // AURORA_HAL_I2C_HAL_HPP
