// =============================================================================
// drivers/virtio/virtio_mmio.cpp — virtio-mmio 传输实现
// =============================================================================
#include "virtio_mmio.hpp"

namespace auroraos {
namespace virtio {

uint32_t VirtioMmioTransport::read32(uint32_t reg_offset) noexcept {
    return *reinterpret_cast<volatile uint32_t*>(base_ + reg_offset);
}

void VirtioMmioTransport::write32(uint32_t reg_offset, uint32_t value) noexcept {
    *reinterpret_cast<volatile uint32_t*>(base_ + reg_offset) = value;
}

uintptr_t virtio_scan_mmio(uintptr_t base, uint32_t count, uint32_t stride, DeviceId expected,
                           uintptr_t fallback) noexcept {
    const uint32_t want = static_cast<uint32_t>(expected);
    for (uint32_t i = 0; i < count; ++i) {
        const uintptr_t addr = base + static_cast<uintptr_t>(i) * stride;
        const auto* magic = reinterpret_cast<volatile uint32_t*>(addr + REG_MAGIC_VALUE);
        const auto* version = reinterpret_cast<volatile uint32_t*>(addr + REG_VERSION);
        const auto* dev_id = reinterpret_cast<volatile uint32_t*>(addr + REG_DEVICE_ID);
        if (*magic == VIRTIO_MMIO_MAGIC && *version == VIRTIO_MMIO_VERSION_MODERN && *dev_id == want) {
            return addr;
        }
    }
    // 未命中：返回板级声明的槽位，让后续 probe 走正常的 fail-safe 分支
    // （no device → skip），不改变「缺设备也能继续 boot」的既有语义。
    return fallback;
}

bool virtio_probe(IVirtioTransport& t, DeviceId expected) noexcept {
    if (t.read32(REG_MAGIC_VALUE) != VIRTIO_MMIO_MAGIC) {
        return false;
    }
    if (t.read32(REG_VERSION) != VIRTIO_MMIO_VERSION_MODERN) {
        return false; // legacy(1) 或非法值一律拒绝
    }
    const uint32_t want = static_cast<uint32_t>(expected);
    return want != 0 && t.read32(REG_DEVICE_ID) == want;
}

void virtio_reset(IVirtioTransport& t) noexcept {
    t.write32(REG_STATUS, 0);
    (void)t.read32(REG_STATUS); // flush posted write，等待复位完成
}

uint32_t virtio_status_get(IVirtioTransport& t) noexcept {
    return t.read32(REG_STATUS);
}

void virtio_status_add(IVirtioTransport& t, uint32_t bits) noexcept {
    t.write32(REG_STATUS, virtio_status_get(t) | bits);
}

bool virtio_negotiate_features(IVirtioTransport& t,
                               uint32_t required_lo, uint32_t required_hi,
                               uint32_t optional_lo, uint32_t optional_hi,
                               uint32_t* out_lo, uint32_t* out_hi) noexcept {
    t.write32(REG_DEVICE_FEATURES_SEL, 0);
    const uint32_t dev_lo = t.read32(REG_DEVICE_FEATURES);
    t.write32(REG_DEVICE_FEATURES_SEL, 1);
    const uint32_t dev_hi = t.read32(REG_DEVICE_FEATURES);

    // VIRTIO_F_VERSION_1（bit 32 → 高字 bit 0）是 modern 契约，必须支持
    const uint32_t version1_hi = 1u << (F_VERSION_1_BIT - 32);
    if ((dev_hi & version1_hi) == 0) {
        return false;
    }
    // required 全集必须被设备支持
    if ((dev_lo & required_lo) != required_lo || (dev_hi & required_hi) != required_hi) {
        return false;
    }

    uint32_t neg_lo = dev_lo & (required_lo | optional_lo);
    uint32_t neg_hi = dev_hi & (required_hi | optional_hi);
    neg_hi |= version1_hi;
    // 绝不协商 ACCESS_PLATFORM（bit 33）：本平台无 IOMMU
    neg_hi &= ~(1u << (F_ACCESS_PLATFORM_BIT - 32));

    t.write32(REG_DRIVER_FEATURES_SEL, 1);
    t.write32(REG_DRIVER_FEATURES, neg_hi);
    t.write32(REG_DRIVER_FEATURES_SEL, 0);
    t.write32(REG_DRIVER_FEATURES, neg_lo);

    if (out_lo) {
        *out_lo = neg_lo;
    }
    if (out_hi) {
        *out_hi = neg_hi;
    }
    return true;
}

uint16_t virtio_queue_configure(IVirtioTransport& t, uint16_t qidx, uint16_t qsize,
                                uint64_t desc_addr, uint64_t avail_addr,
                                uint64_t used_addr) noexcept {
    t.write32(REG_QUEUE_SEL, qidx);
    const uint32_t max = t.read32(REG_QUEUE_NUM_MAX);
    if (max == 0) {
        return 0; // 队列不存在
    }
    uint32_t n = qsize;
    if (n > max) {
        n = max;
    }

    t.write32(REG_QUEUE_NUM, n);
    t.write32(REG_QUEUE_DESC_LOW, static_cast<uint32_t>(desc_addr));
    t.write32(REG_QUEUE_DESC_HIGH, static_cast<uint32_t>(desc_addr >> 32));
    t.write32(REG_QUEUE_DRIVER_LOW, static_cast<uint32_t>(avail_addr));
    t.write32(REG_QUEUE_DRIVER_HIGH, static_cast<uint32_t>(avail_addr >> 32));
    t.write32(REG_QUEUE_DEVICE_LOW, static_cast<uint32_t>(used_addr));
    t.write32(REG_QUEUE_DEVICE_HIGH, static_cast<uint32_t>(used_addr >> 32));
    t.write32(REG_QUEUE_READY, 1);
    return static_cast<uint16_t>(n);
}

bool virtio_features_ok(IVirtioTransport& t) noexcept {
    virtio_status_add(t, STATUS_FEATURES_OK);
    // 设备可能拒绝协商结果：重读 Status 确认 FEATURES_OK 仍然置位
    return (virtio_status_get(t) & STATUS_FEATURES_OK) != 0;
}

void virtio_driver_ok(IVirtioTransport& t) noexcept {
    virtio_status_add(t, STATUS_DRIVER_OK);
}

uint32_t virtio_ack_interrupt(IVirtioTransport& t) noexcept {
    const uint32_t pending = t.read32(REG_INTERRUPT_STATUS);
    t.write32(REG_INTERRUPT_ACK, pending & 0x3);
    return pending;
}

} // namespace virtio
} // namespace auroraos
