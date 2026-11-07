// =============================================================================
// drivers/virtio/virtio_mmio.hpp
//
// virtio-mmio 传输实现：
//   1. VirtioMmioTransport — 纯基址适配器（read32/write32 = base+offset），
//      唯一直接触碰硬件寄存器的类。
//   2. virtio_* 自由函数 — 传输无关的初始化状态机（probe/协商/队列配置/
//      DRIVER_OK/中断 ack），全部经 IVirtioTransport 操作，host 测试可注入
//      FakeTransport 全覆盖。
//
// 仅支持 modern（Version==2）设备；legacy（Version==1）明确拒绝。
// =============================================================================
#ifndef AURORA_VIRTIO_MMIO_HPP
#define AURORA_VIRTIO_MMIO_HPP

#include <stdint.h>
#include "virtio_defs.hpp"
#include "virtio_transport.hpp"

namespace auroraos {
namespace virtio {

class VirtioMmioTransport final : public IVirtioTransport {
public:
    explicit VirtioMmioTransport(uintptr_t mmio_base) noexcept : base_(mmio_base) {}

    uint32_t read32(uint32_t reg_offset) noexcept override;
    void write32(uint32_t reg_offset, uint32_t value) noexcept override;

    uintptr_t mmio_base() const noexcept {
        return base_;
    }

private:
    uintptr_t base_;
};

// ---- 传输无关初始化状态机 ----

// 校验 Magic(0x74726976) / Version(==2) / DeviceID(==expected, 非 0)
bool virtio_probe(IVirtioTransport& t, DeviceId expected) noexcept;

// 复位：Status=0 并读回（flush posted write）
void virtio_reset(IVirtioTransport& t) noexcept;

uint32_t virtio_status_get(IVirtioTransport& t) noexcept;
void virtio_status_add(IVirtioTransport& t, uint32_t bits) noexcept;

// feature 协商：required 必须全部被设备支持，optional 按交集取。
// 始终强制 VIRTIO_F_VERSION_1，绝不置 ACCESS_PLATFORM（无 IOMMU）。
// out_lo/out_hi 可空，非空时回写最终协商值。失败返回 false。
bool virtio_negotiate_features(IVirtioTransport& t,
                               uint32_t required_lo, uint32_t required_hi,
                               uint32_t optional_lo, uint32_t optional_hi,
                               uint32_t* out_lo, uint32_t* out_hi) noexcept;

// 队列就绪配置：选队列、钳制容量、写三段地址、置 QueueReady。
// qsize 为驱动期望值，实际写入 min(qsize, QueueNumMax)；返回实际值，0 表示失败。
uint16_t virtio_queue_configure(IVirtioTransport& t, uint16_t qidx, uint16_t qsize,
                                uint64_t desc_addr, uint64_t avail_addr,
                                uint64_t used_addr) noexcept;

// FEATURES_OK 置位后重读确认（设备可能拒绝）
bool virtio_features_ok(IVirtioTransport& t) noexcept;

void virtio_driver_ok(IVirtioTransport& t) noexcept;

// 读 InterruptStatus 并写 InterruptACK。轮询驱动也须调用，避免语义挂死。
uint32_t virtio_ack_interrupt(IVirtioTransport& t) noexcept;

} // namespace virtio
} // namespace auroraos

#endif // AURORA_VIRTIO_MMIO_HPP
