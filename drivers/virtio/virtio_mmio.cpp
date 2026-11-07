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

// TEMP-DIAG: 十六进制打印（诊断 RV32 virtio 探测，稍后删除）
extern "C" void uart_puts(const char* s);
namespace {
void dbg_hex(const char* label, uint32_t v) {
    char buf[9];
    const char* hex = "0123456789abcdef";
    int i = 0;
    for (int s = 28; s >= 0; s -= 4) {
        buf[i++] = hex[(v >> s) & 0xFu];
    }
    buf[i] = '\0';
    uart_puts("[DBG] ");
    uart_puts(label);
    uart_puts(buf);
    uart_puts("\n");
}
} // namespace

bool virtio_probe(IVirtioTransport& t, DeviceId expected) noexcept {
    dbg_hex("magic=", t.read32(REG_MAGIC_VALUE));
    dbg_hex("ver=", t.read32(REG_VERSION));
    dbg_hex("id=", t.read32(REG_DEVICE_ID));
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
