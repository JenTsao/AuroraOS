// =============================================================================
// drivers/virtio/virtio_queue.cpp — split virtqueue 环管理实现
// 热路径零动态分配：描述符空闲链复用 desc[].next 字段，不引入额外内存。
// =============================================================================
#include "virtio_queue.hpp"

namespace auroraos {
namespace virtio {

namespace {

// 生产/消费屏障：保证描述符内容先于 avail->idx 更新被设备看到（wmb），
// 以及 used->idx 更新先于读取 used 元素内容（rmb）。
inline void dma_wmb() noexcept {
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

inline void dma_rmb() noexcept {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

} // namespace

bool VirtQueue::init(const Rings& rings, uint16_t queue_size, uint16_t queue_index,
                     IVirtioTransport& transport) noexcept {
    if (!rings.desc || !rings.avail || !rings.used || queue_size == 0 ||
        queue_size > VIRTQ_MAX_QUEUE_SIZE) {
        return false;
    }

    rings_ = rings;
    queue_size_ = queue_size;
    queue_index_ = queue_index;
    transport_ = &transport;

    // 空闲链：desc[i].next 串起 0→1→…→size-1，free_head_=0。
    for (uint16_t i = 0; i < queue_size_; ++i) {
        rings_.desc[i].addr = 0;
        rings_.desc[i].len = 0;
        rings_.desc[i].flags = 0;
        rings_.desc[i].next = static_cast<uint16_t>(i + 1);
    }
    free_head_ = 0;
    free_count_ = queue_size_;

    rings_.avail->flags = 0;
    rings_.avail->idx = 0;
    rings_.used->flags = 0;
    rings_.used->idx = 0;
    last_used_ = 0;
    return true;
}

int VirtQueue::add_buffer(const BufSeg* segs, uint16_t n) noexcept {
    if (!segs || n == 0 || n > queue_size_ || free_count_ < n) {
        return -1;
    }

    // 第一步：从空闲链取出 n 个描述符下标（此时尚未覆写 next，链走完整）
    uint16_t chain[VIRTQ_MAX_QUEUE_SIZE];
    for (uint16_t i = 0; i < n; ++i) {
        chain[i] = free_head_;
        free_head_ = rings_.desc[free_head_].next;
    }
    free_count_ = static_cast<uint16_t>(free_count_ - n);

    // 第二步：填充缓冲段并按 NEXT 标志链接成设备可遍历的描述符链
    for (uint16_t i = 0; i < n; ++i) {
        VirtqDesc& d = rings_.desc[chain[i]];
        d.addr = segs[i].addr;
        d.len = segs[i].len;
        uint16_t flags = segs[i].device_writable ? VRING_DESC_F_WRITE : 0;
        if (i + 1 < n) {
            flags |= VRING_DESC_F_NEXT;
            d.next = chain[i + 1];
        } else {
            d.next = 0;
        }
        d.flags = flags;
    }

    // 描述符内容先于 avail->idx 对设备可见
    dma_wmb();
    rings_.avail->ring[rings_.avail->idx % queue_size_] = chain[0];
    dma_wmb();
    rings_.avail->idx = static_cast<uint16_t>(rings_.avail->idx + 1);
    return chain[0];
}

int VirtQueue::pop_used(uint32_t* len) noexcept {
    if (!rings_.used || last_used_ == rings_.used->idx) {
        return -1;
    }

    dma_rmb();
    const VirtqUsedElem elem = rings_.used->ring[last_used_ % queue_size_];
    last_used_ = static_cast<uint16_t>(last_used_ + 1);

    free_chain(static_cast<uint16_t>(elem.id));
    if (len) {
        *len = elem.len;
    }
    return static_cast<int>(elem.id);
}

void VirtQueue::notify() noexcept {
    if (transport_) {
        transport_->write32(REG_QUEUE_NOTIFY, queue_index_);
    }
}

void VirtQueue::set_notify_callback(NotifyCb cb, void* ctx) noexcept {
    notify_cb_ = cb;
    notify_ctx_ = ctx;
}

void VirtQueue::on_interrupt() noexcept {
    // ISR 预留入口：ack 传输层中断由调用方（未来的中断分发框架）负责，
    // 这里只派发通知回调。当前无 IRQ 接入，暂无调用方。
    if (notify_cb_) {
        notify_cb_(notify_ctx_, *this);
    }
}

void VirtQueue::free_chain(uint16_t head) noexcept {
    uint16_t cur = head;
    while (true) {
        const uint16_t next = rings_.desc[cur].next;
        const bool has_next = (rings_.desc[cur].flags & VRING_DESC_F_NEXT) != 0;
        rings_.desc[cur].flags = 0;
        rings_.desc[cur].addr = 0;
        rings_.desc[cur].len = 0;
        rings_.desc[cur].next = free_head_;
        free_head_ = cur;
        ++free_count_;
        if (!has_next) {
            break;
        }
        cur = next;
    }
}

} // namespace virtio
} // namespace auroraos
