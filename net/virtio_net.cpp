// =============================================================================
// net/virtio_net.cpp — virtio-net 网卡驱动实现
// =============================================================================
#include "virtio_net.hpp"
#include "board.h"
#include "../drivers/virtio/virtio_mmio.hpp"

namespace auroraos {
namespace virtio {

VirtioNet& VirtioNet::instance() {
    // 槽位按设备类型扫描（见 virtio_scan_mmio 注释），board.h 的 SLOT 只作回退。
    static VirtioMmioTransport mmio(auroraos::virtio::virtio_scan_mmio(
        BOARD_VIRTIO_MMIO_BASE, BOARD_VIRTIO_MMIO_COUNT, BOARD_VIRTIO_MMIO_STRIDE, auroraos::virtio::DeviceId::Net,
        BOARD_VIRTIO_MMIO_BASE + BOARD_VIRTIO_NET_SLOT * BOARD_VIRTIO_MMIO_STRIDE));
    static VirtioNet dev(mmio);
    return dev;
}

bool VirtioNet::post_rx_buffer(uint16_t slot) noexcept {
    uint8_t* base = rx_buf_[slot];
    // NetHdr 与帧区连续，单描述符覆盖（每槽 1 描述符，8 槽 = 8 描述符 = 队列容量）
    BufSeg segs[1] = {
        {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(base)), static_cast<uint32_t>(sizeof(NetHdr) + MAX_FRAME),
         true},
    };
    dma_sync_for_device(base, sizeof(NetHdr) + MAX_FRAME);
    const int head = rxq_.add_buffer(segs, 1);
    if (head < 0 || head >= QUEUE_SIZE) {
        return false;
    }
    rx_head_slot_[head] = static_cast<int16_t>(slot);
    ++rx_posted_;
    return true;
}

bool VirtioNet::init() {
    if (!virtio_probe(transport_, DeviceId::Net)) {
        return false;
    }
    probed_ = true;

    virtio_reset(transport_);
    virtio_status_add(transport_, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

    // 可选 NET_F_MAC（配置空间 MAC 可读）；MRG_RXBUF 绝不协商（10B 头契约）
    uint32_t feat_lo = 0;
    if (!virtio_negotiate_features(transport_, 0, 0, 1u << NET_F_MAC_BIT, 0, &feat_lo, nullptr) ||
        !virtio_features_ok(transport_)) {
        virtio_status_add(transport_, STATUS_FAILED);
        return false;
    }

    const uint64_t rx_desc = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(rx_desc_));
    const uint64_t rx_avail = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&rx_avail_));
    const uint64_t rx_used = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&rx_used_));
    const uint64_t tx_desc = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(tx_desc_));
    const uint64_t tx_avail = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&tx_avail_));
    const uint64_t tx_used = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&tx_used_));

    const uint16_t rx_sz = virtio_queue_configure(transport_, RX_QUEUE_IDX, QUEUE_SIZE,
                                                  rx_desc, rx_avail, rx_used);
    const uint16_t tx_sz = virtio_queue_configure(transport_, TX_QUEUE_IDX, QUEUE_SIZE,
                                                  tx_desc, tx_avail, tx_used);
    if (rx_sz == 0 || tx_sz == 0) {
        virtio_status_add(transport_, STATUS_FAILED);
        return false;
    }

    VirtQueue::Rings rx_rings{rx_desc_, &rx_avail_, &rx_used_};
    VirtQueue::Rings tx_rings{tx_desc_, &tx_avail_, &tx_used_};
    if (!rxq_.init(rx_rings, rx_sz, RX_QUEUE_IDX, transport_) ||
        !txq_.init(tx_rings, tx_sz, TX_QUEUE_IDX, transport_)) {
        virtio_status_add(transport_, STATUS_FAILED);
        return false;
    }

    // MAC：协商了 NET_F_MAC 则从配置空间偏移 0 读取，否则用本地管理位默认值
    uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    if ((feat_lo & (1u << NET_F_MAC_BIT)) != 0) {
        const uint32_t lo = transport_.read32(REG_CONFIG_SPACE + 0);
        const uint32_t hi = transport_.read32(REG_CONFIG_SPACE + 4);
        mac[0] = static_cast<uint8_t>(lo);
        mac[1] = static_cast<uint8_t>(lo >> 8);
        mac[2] = static_cast<uint8_t>(lo >> 16);
        mac[3] = static_cast<uint8_t>(lo >> 24);
        mac[4] = static_cast<uint8_t>(hi);
        mac[5] = static_cast<uint8_t>(hi >> 8);
    }
    set_mac_address(mac);

    // 预投全部 RX 缓冲
    for (uint16_t i = 0; i < QUEUE_SIZE; ++i) {
        rx_head_slot_[i] = -1;
    }
    for (uint16_t i = 0; i < QUEUE_SIZE; ++i) {
        if (!post_rx_buffer(i)) {
            virtio_status_add(transport_, STATUS_FAILED);
            return false;
        }
    }
    rxq_.notify();

    virtio_driver_ok(transport_);
    link_up_ = true;
    return true;
}

int VirtioNet::receive_frame(uint8_t* buffer, int max_len) {
    if (!buffer || max_len <= 0 || !link_up_) {
        return 0;
    }

    uint32_t len = 0;
    const int head = rxq_.pop_used(&len);
    if (head < 0 || head >= QUEUE_SIZE) {
        return 0; // 无数据（非法 used 元素已在 pop_used 内丢弃）
    }

    const int16_t slot = rx_head_slot_[head];
    rx_head_slot_[head] = -1;
    if (slot < 0) {
        return 0;
    }
    --rx_posted_;

    uint8_t* base = rx_buf_[slot];
    dma_sync_for_cpu(base, sizeof(NetHdr) + MAX_FRAME);

    // used len 含 NetHdr(10)；帧长 = len - 10
    int frame_len = (len > sizeof(NetHdr)) ? static_cast<int>(len - sizeof(NetHdr)) : 0;
    if (frame_len > static_cast<int>(MAX_FRAME)) {
        frame_len = static_cast<int>(MAX_FRAME);
    }
    int copy = frame_len < max_len ? frame_len : max_len;
    for (int i = 0; i < copy; ++i) {
        buffer[i] = base[sizeof(NetHdr) + i];
    }

    // 取完立即补投，维持收包缓冲池；补投失败说明描述符池已不可恢复
    // （刚回收一个却取不到），显式停用链路而不是静默断流。
    if (!post_rx_buffer(static_cast<uint16_t>(slot))) {
        virtio_status_add(transport_, STATUS_FAILED);
        link_up_ = false;
    }
    rxq_.notify();
    return copy;
}

bool VirtioNet::send_frame(const uint8_t* buffer, int len) {
    if (!buffer || len <= 0 || len > static_cast<int>(MAX_FRAME) || !link_up_) {
        return false;
    }

    LockGuard guard(tx_mutex_);

    // NetHdr 全零：无 GSO/校验和卸载
    for (uint32_t i = 0; i < sizeof(NetHdr); ++i) {
        reinterpret_cast<uint8_t*>(&tx_hdr_)[i] = 0;
    }
    for (int i = 0; i < len; ++i) {
        tx_frame_[i] = buffer[i];
    }

    BufSeg segs[2] = {
        {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&tx_hdr_)), sizeof(NetHdr), false},
        {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(tx_frame_)), static_cast<uint32_t>(len), false},
    };
    dma_sync_for_device(&tx_hdr_, sizeof(tx_hdr_));
    dma_sync_for_device(tx_frame_, static_cast<uint32_t>(len));

    const int head = txq_.add_buffer(segs, 2);
    if (head < 0) {
        return false;
    }
    txq_.notify();

    uint32_t done_len = 0;
    for (uint32_t spin = 0; spin < MAX_POLL_SPINS; ++spin) {
        const int done_head = txq_.pop_used(&done_len);
        if (done_head >= 0) {
            if (done_head != head) {
                // 孤儿完成：tx_hdr_/tx_frame_ 有被迟到 DMA 撕裂的风险，停用链路
                virtio_status_add(transport_, STATUS_FAILED);
                link_up_ = false;
                return false; // 单请求在途：完成必属本帧
            }
            return true;
        }
    }
    // 超时：不挂死；停用链路防止 tx_hdr_/tx_frame_ 被迟到 DMA 打坏
    virtio_status_add(transport_, STATUS_FAILED);
    link_up_ = false;
    return false;
}

} // namespace virtio
} // namespace auroraos
