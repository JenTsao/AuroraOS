// =============================================================================
// drivers/virtio/virtio_queue.hpp
//
// split virtqueue 核心：描述符链组装、avail/used 环推进、16 位回绕、满环拒绝。
// 纯算法 + 注入式缓冲（Rings 由调用方提供，固件静态分配、host 测试用栈数组），
// 不直接访问 MMIO；仅 notify() 经 IVirtioTransport 写 QueueNotify 寄存器。
//
// DMA 前提：描述符/avail/used 环与数据缓冲均为物理连续内存。QEMU 无 cache
// 建模，直接可用；真实硬件需在生产/消费前后做 cache 维护（dma_sync_*() 留桩）。
// =============================================================================
#ifndef AURORA_VIRTIO_QUEUE_HPP
#define AURORA_VIRTIO_QUEUE_HPP

#include <stdint.h>
#include "virtio_defs.hpp"
#include "virtio_transport.hpp"

namespace auroraos {
namespace virtio {

// 环大小上限（每队列描述符数）。资源受限目标按需调小。
constexpr uint16_t VIRTQ_MAX_QUEUE_SIZE = 8;

// avail ring 完整形状（ring 槽位数 = 队列大小，≤ VIRTQ_MAX_QUEUE_SIZE）。
// used_event 仅作布局占位：不协商 VIRTIO_F_RING_EVENT_IDX。
struct AvailRing {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTQ_MAX_QUEUE_SIZE];
    uint16_t used_event;
};

struct UsedRing {
    uint16_t flags;
    uint16_t idx;
    VirtqUsedElem ring[VIRTQ_MAX_QUEUE_SIZE];
    uint16_t avail_event;
};

class VirtQueue {
public:
    struct Rings {
        VirtqDesc* desc;   // queue_size 个
        AvailRing* avail;
        UsedRing* used;
    };

    // 队列初始化：建立描述符空闲链、清零索引。queue_size 取自设备
    // QueueNumMax 钳制后的实际值。失败（非法参数）返回 false。
    bool init(const Rings& rings, uint16_t queue_size, uint16_t queue_index,
              IVirtioTransport& transport) noexcept;

    // 提交一段描述符链（segs[0..n-1] 依次链接）。返回链头描述符下标（≥0），
    // 空闲描述符不足或参数非法返回 -1。n==0 视为非法。
    int add_buffer(const BufSeg* segs, uint16_t n) noexcept;

    // 取一个已完成元素：返回链头下标并释放整条链；队列为空返回 -1。
    // len 可空，非空时回写设备侧完成字节数。
    int pop_used(uint32_t* len) noexcept;

    // 通知设备有新 avail 元素（写 QueueNotify）。轮询驱动同样需要调用。
    void notify() noexcept;

    uint16_t free_desc_count() const noexcept {
        return free_count_;
    }

    uint16_t queue_size() const noexcept {
        return queue_size_;
    }

    // 环缓冲只读访问（host 测试的设备模拟器绑定用）
    const Rings& rings() const noexcept {
        return rings_;
    }

    // ---- 中断通知预留接口（当前无 IRQ 接入，仅桩）----
    using NotifyCb = void (*)(void* ctx, VirtQueue& q);
    void set_notify_callback(NotifyCb cb, void* ctx) noexcept;
    // 供未来 ISR 调用：ack 传输层中断后派发回调。当前无调用方。
    void on_interrupt() noexcept;

private:
    void free_chain(uint16_t head) noexcept;

    Rings rings_{};
    uint16_t queue_size_ = 0;
    uint16_t queue_index_ = 0;
    uint16_t free_head_ = 0;
    uint16_t free_count_ = 0;
    uint16_t last_used_ = 0; // 已消费的 used->idx 快照
    // 描述符在用位图（queue_size ≤ VIRTQ_MAX_QUEUE_SIZE = 8，8 位足够）。
    // 用于识别设备写入的 used 元素是否合法：非在用/越界的链头一律拒绝，
    // 防止双重释放与越界访问破坏描述符表。
    uint8_t in_use_mask_ = 0;
    IVirtioTransport* transport_ = nullptr;
    NotifyCb notify_cb_ = nullptr;
    void* notify_ctx_ = nullptr;
};

// DMA 同步钩子：QEMU 无需操作；真实硬件需按 cache 一致性维护（见文件头）。
inline void dma_sync_for_device(const void* /*addr*/, uint32_t /*len*/) noexcept {}
inline void dma_sync_for_cpu(const void* /*addr*/, uint32_t /*len*/) noexcept {}

} // namespace virtio
} // namespace auroraos

#endif // AURORA_VIRTIO_QUEUE_HPP
