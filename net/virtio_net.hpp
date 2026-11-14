// =============================================================================
// net/virtio_net.hpp
//
// virtio-net 网卡驱动：实现 NetDevice 契约（init/receive_frame/send_frame），
// 供 lwIP netif（apps/net_app.cpp）挂接。放 net/ 而非 drivers/：StellarisEth
// （net/eth_driver.hpp）同为网卡驱动先例，且 drivers→net 是架构层禁止边。
//
// 轮询模式：RX 由后台任务轮询收包（receive_frame 取帧后即刻补投缓冲），
// TX 同步发送（单请求在途，自旋等待完成）。未协商 NET_F_MRG_RXBUF，
// NetHdr 为 12 字节（Modern）；MAC 取自配置空间（VIRTIO_NET_F_MAC），否则用本地
// 管理位默认地址。
// =============================================================================
#ifndef AURORA_VIRTIO_NET_HPP
#define AURORA_VIRTIO_NET_HPP

#include <stdint.h>
#include "net_device.hpp"
#include "../kernel/core/mutex.hpp"
#include "../drivers/virtio/virtio_defs.hpp"
#include "../drivers/virtio/virtio_queue.hpp"

namespace auroraos {
namespace virtio {

class VirtioNet : public NetDevice {
public:
    static constexpr uint16_t QUEUE_SIZE = 8;
    static constexpr uint32_t MAX_FRAME = 1514;
    static constexpr uint32_t MAX_POLL_SPINS = 1000000;
    static constexpr uint16_t RX_QUEUE_IDX = 0;
    static constexpr uint16_t TX_QUEUE_IDX = 1;

    // 收发描述符环与帧缓冲由 init() 在探测成功后填写，构造期不预填
    // （静态实例在 .bss 里本就为零）。cppcheck 的 uninitMemberVar 属静态误报。
    // cppcheck-suppress uninitMemberVar
    explicit VirtioNet(IVirtioTransport& transport) noexcept : transport_(transport) {}

    // 板级工厂：按 BOARD_VIRTIO_* 常量定位传输口（见 virtio_net.cpp）
    static VirtioNet& instance();

    bool init() override;
    int receive_frame(uint8_t* buffer, int max_len) override;
    bool send_frame(const uint8_t* buffer, int len) override;

    // probe 是否命中（区分「无设备跳过」与「初始化失败」两类日志）
    bool probed() const noexcept {
        return probed_;
    }

    VirtQueue& rx_queue() noexcept {
        return rxq_;
    }
    VirtQueue& tx_queue() noexcept {
        return txq_;
    }

    // 测试观察用：已补投的 RX 缓冲数（应始终等于 QUEUE_SIZE - 在途数）
    uint32_t rx_posted() const noexcept {
        return rx_posted_;
    }

private:
    // 补投一个 RX 缓冲（init 与收帧后复用同一路径）
    bool post_rx_buffer(uint16_t slot) noexcept;

    IVirtioTransport& transport_;
    VirtQueue rxq_;
    VirtQueue txq_;
    alignas(4096) VirtqDesc rx_desc_[QUEUE_SIZE];
    alignas(4096) AvailRing rx_avail_;
    alignas(4096) UsedRing rx_used_;
    alignas(4096) VirtqDesc tx_desc_[QUEUE_SIZE];
    alignas(4096) AvailRing tx_avail_;
    alignas(4096) UsedRing tx_used_;

    // RX 缓冲：NetHdr(12) + 帧(1514)，逐槽静态分配
    alignas(4096) uint8_t rx_buf_[QUEUE_SIZE][sizeof(NetHdr) + MAX_FRAME];
    int16_t rx_head_slot_[QUEUE_SIZE];
    uint32_t rx_posted_ = 0;
    bool probed_ = false;

    // TX 单缓冲（单请求在途）
    alignas(4) NetHdr tx_hdr_;
    alignas(4) uint8_t tx_frame_[MAX_FRAME];
    Mutex tx_mutex_;
};

} // namespace virtio
} // namespace auroraos

#endif // AURORA_VIRTIO_NET_HPP
