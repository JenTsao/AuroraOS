#ifndef AURORA_HAL_SPI_HAL_HPP
#define AURORA_HAL_SPI_HAL_HPP

#include <stdint.h>
#include <stddef.h>

namespace auroraos {
namespace hal {

class ISpiHal {
public:
    virtual ~ISpiHal() = default;

    // 发送一个字节
    virtual void transmit_byte(uint8_t byte) = 0;

    // 发送多个字节（阻塞等待）
    virtual void transmit(const uint8_t* data, size_t len) = 0;

    // 接收多个字节
    virtual void receive(uint8_t* data, size_t len) = 0;

    // 发送大块数据（推荐使用 DMA/中断，异步操作）
    virtual void transmit_dma(const uint8_t* data, size_t len) = 0;

    // 等待 DMA/异步传输完成（或直接使用回调/信号）
    virtual void wait_transmit_complete() = 0;
};

// 获取设备级 SPI 实例（由 Board 提供，根据总线 ID 区分）
ISpiHal* get_spi_hal(int bus_id);

} // namespace hal
} // namespace auroraos

#endif // AURORA_HAL_SPI_HAL_HPP
