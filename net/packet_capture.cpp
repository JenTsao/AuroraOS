#include "packet_capture.hpp"
#include "protocol_analyzer.hpp"
#include "eth_driver.hpp"
#include <string.h>

// ---- 全局挂钟 tick（SysTick_Handler 递增，1ms/tick） ----
extern volatile uint32_t tick_count;

// ============================================================
// 真实时间戳注入
//   tick_count 在 boot/interrupts.cpp 的 SysTick_Handler 中递增，
//   不依赖 TimerManager 的单例初始化时序，保证始终可用。
//   TICK_RATE_HZ 默认为 1000 (Kconfig)，因此 1 tick = 1 ms。
// ============================================================

static inline uint32_t wall_ms() {
    return tick_count;
}

static inline void fill_ts(PacketBuffer* p) {
    uint32_t ms = wall_ms();
    p->timestamp_sec = ms / 1000;
    p->timestamp_usec = (ms % 1000) * 1000;
}

// ============================================================
// init — 挂载 /dev/pcap0
// ============================================================

void PacketCapture::init() {
    is_open_ = false;
    head_ = 0;
    tail_ = 0;
    memset(&stats_, 0, sizeof(stats_));
    memset(&filter_, 0, sizeof(filter_));
    filter_.filter_or = true; // OR 模式且无规则 = 放行所有
    VfsManager::instance().mount("/dev/pcap0", this);
}

// ============================================================
// RX / TX 钩子（ethernetif.cpp 调用）
// ============================================================

void PacketCapture::tap_rx_packet(const uint8_t* buffer, int len) {
    if (!is_open_ || len <= 0 || len > MAX_FRAME)
        return;

    // 1. BPF 过滤
    if (!ProtocolAnalyzer::match_filter(buffer, len, filter_)) {
        LockGuard lk(stats_lock_);
        stats_.packets_filtered++;
        return;
    }

    // 2. 从内存池分配
    PacketBuffer* p = pool_.allocate();
    if (!p) {
        LockGuard lk(stats_lock_);
        stats_.packets_dropped++;
        return;
    }

    p->length = static_cast<uint32_t>(len);
    fill_ts(p);
    memcpy(p->data, buffer, len);

    // 3. 推入环形缓冲
    {
        LockGuard lk(ring_lock_);
        int next = (head_ + 1) % RING_SIZE;
        if (next == tail_) {
            // 缓冲满 → 覆盖最老包
            pool_.deallocate(ring_[tail_]);
            tail_ = (tail_ + 1) % RING_SIZE;
            stats_.packets_dropped++;
        }
        ring_[head_] = p;
        head_ = next;

        // 更新峰值占用量
        int used = (head_ >= tail_) ? (head_ - tail_) : (RING_SIZE - tail_ + head_);
        if (static_cast<uint32_t>(used) > stats_.peak_ring_usage)
            stats_.peak_ring_usage = static_cast<uint32_t>(used);
    }

    LockGuard lk(stats_lock_);
    stats_.packets_captured++;
    stats_.bytes_captured += static_cast<uint32_t>(len);
}

void PacketCapture::tap_tx_packet(const uint8_t* buffer, int len) {
    tap_rx_packet(buffer, len); // 合并路径：TX 也捕获
}

// ============================================================
// 过滤器 & 统计
// ============================================================

void PacketCapture::set_filter(const BpfFilter& f) {
    LockGuard lk(filter_lock_);
    filter_ = f;
}

CaptureStats PacketCapture::get_stats() const {
    LockGuard lk(stats_lock_);
    return stats_;
}

void PacketCapture::reset_stats() {
    LockGuard lk(stats_lock_);
    memset(&stats_, 0, sizeof(stats_));
}

// ============================================================
// VNode: /dev/pcap0 — 输出兼容 Wireshark .pcap 格式
//
// 读取协议：
//   open  → offset = 0
//   read(offset=0)  → PCAP 全局文件头 (24 字节)，VFS 将 offset 设为 24
//   read(offset=24) → 数据包记录 (16 字节头 + 帧数据)，VFS 递增 offset
//   ...
//   read(offset=...) → 返回 0 表示当前无包（非阻塞，调用者应重试）
// ============================================================

int PacketCapture::open_file(const char*, int, void**) {
    if (is_open_)
        return -1; // 单读取者

    is_open_ = true;

    {
        LockGuard lk(ring_lock_);
        while (tail_ != head_) {
            pool_.deallocate(ring_[tail_]);
            tail_ = (tail_ + 1) % RING_SIZE;
        }
    }
    reset_stats();
    return 0;
}

int PacketCapture::close_file(void*) {
    is_open_ = false;
    {
        LockGuard lk(ring_lock_);
        while (tail_ != head_) {
            pool_.deallocate(ring_[tail_]);
            tail_ = (tail_ + 1) % RING_SIZE;
        }
    }
    StellarisEth::instance().set_promiscuous_mode(false);
    return 0;
}

int PacketCapture::read(char* buf, int len, int offset, void*) {
    if (!is_open_ || len <= 0)
        return 0;

    constexpr int HDR_SZ = static_cast<int>(sizeof(pcap_file_hdr_t)); // 24

    // ---- 路径 A: 尚未发送全局头 (offset < sizeof(global_header)) ----
    if (offset < HDR_SZ) {
        if (len < HDR_SZ)
            return 0;
        pcap_file_hdr_t fh;
        fh.magic_number = 0xa1b2c3d4;
        fh.version_major = 2;
        fh.version_minor = 4;
        fh.thiszone = 0;
        fh.sigfigs = 0;
        fh.snaplen = SNAPLEN_DEFAULT;
        fh.network = 1;
        memcpy(buf, &fh, HDR_SZ);
        return HDR_SZ;
    }

    // ---- 路径 B: 出队一个数据包 ----
    PacketBuffer* p = nullptr;
    {
        LockGuard lk(ring_lock_);
        if (head_ == tail_)
            return 0; // 无数据
        p = ring_[tail_];
        tail_ = (tail_ + 1) % RING_SIZE;
    }

    uint32_t pkt_len = p->length;
    int rec_size = static_cast<int>(sizeof(pcap_rec_hdr_t) + pkt_len);

    if (len < rec_size) {
        pool_.deallocate(p); // 调用者缓冲太小，丢弃
        return -1;
    }

    pcap_rec_hdr_t rh;
    rh.ts_sec = p->timestamp_sec;
    rh.ts_usec = p->timestamp_usec;
    rh.incl_len = pkt_len;
    rh.orig_len = pkt_len;

    memcpy(buf, &rh, sizeof(rh));
    memcpy(buf + sizeof(rh), p->data, pkt_len);
    pool_.deallocate(p);
    return rec_size;
}

int PacketCapture::write(const char*, int, int, void*) {
    return -1; // 只读
}

int PacketCapture::ioctl(int request, void* arg, void*) {
    switch (request) {
    case IOCTL_SET_FILTER:
        if (arg) {
            set_filter(*static_cast<BpfFilter*>(arg));
            return 0;
        }
        break;
    case IOCTL_GET_FILTER:
        if (arg) {
            LockGuard lk(filter_lock_);
            *static_cast<BpfFilter*>(arg) = filter_;
            return 0;
        }
        break;
    case IOCTL_ENABLE_PROMISC:
        StellarisEth::instance().set_promiscuous_mode(true);
        return 0;
    case IOCTL_DISABLE_PROMISC:
        StellarisEth::instance().set_promiscuous_mode(false);
        return 0;
    case IOCTL_GET_STATS:
        if (arg) {
            *static_cast<CaptureStats*>(arg) = get_stats();
            return 0;
        }
        break;
    case IOCTL_RESET_STATS:
        reset_stats();
        return 0;
    }
    return -1;
}
