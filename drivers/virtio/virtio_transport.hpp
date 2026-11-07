// =============================================================================
// drivers/virtio/virtio_transport.hpp
//
// virtio 传输层抽象：把 MMIO 寄存器读写收敛到唯一接口，virtqueue 与设备驱动
// 只依赖本接口。host 单元测试用 FakeTransport（tests/stubs/）注入普通内存，
// 固件用 VirtioMmioTransport（virtio_mmio.hpp）访问真实 MMIO。
// =============================================================================
#ifndef AURORA_VIRTIO_TRANSPORT_HPP
#define AURORA_VIRTIO_TRANSPORT_HPP

#include <stdint.h>

namespace auroraos {
namespace virtio {

class IVirtioTransport {
public:
    virtual ~IVirtioTransport() = default;

    virtual uint32_t read32(uint32_t reg_offset) noexcept = 0;
    virtual void write32(uint32_t reg_offset, uint32_t value) noexcept = 0;
};

} // namespace virtio
} // namespace auroraos

#endif // AURORA_VIRTIO_TRANSPORT_HPP
