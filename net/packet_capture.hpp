#ifndef PACKET_CAPTURE_HPP
#define PACKET_CAPTURE_HPP

#include <stdint.h>
#include <stddef.h>
#include "../kernel/memory_pool.hpp"
#include "../kernel/mutex.hpp"
#include "../vfs/vfs.hpp"

// ---- BPF 风格过滤器 ----

class ProtocolAnalyzer;  // forward

// 单个过滤字段的启用+数据组合
// MAC 过滤
struct MacFilter { bool enabled; uint8_t addr[6]; };
// IP 过滤（含 CIDR 掩码）
struct IpFilter  { bool enabled; uint32_t addr; uint32_t mask; };
// 端口范围过滤
struct PortFilter{ bool enabled; uint16_t lo; uint16_t hi; };

// BPF 风格复合过滤器 — 每个字段独立启用/禁用
struct BpfFilter {
    // L2 — MAC
    MacFilter  mac_src;
    MacFilter  mac_dst;

    // L3 — IP (network byte order)
    IpFilter   ip_src;
    IpFilter   ip_dst;

    // L3 — 协议位图 (bit 6=TCP, bit 17=UDP, bit 1=ICMP, 0=任意)
    bool       proto_enabled;
    uint32_t   proto_bitmask;   // 0 表示不匹配任何协议

    // L4 — 端口范围 (network byte order, lo ≤ port ≤ hi)
    PortFilter port_src;
    PortFilter port_dst;

    // L4 — TCP flags (SYN=0x02, ACK=0x10, FIN=0x01, RST=0x04, PSH=0x08)
    bool       tcp_flags_enabled;
    uint8_t    tcp_flags_val;   // 预期标志位

    // 复合匹配模式
    bool       filter_or;       // true=任一字段匹配即放行, false=全部匹配才放行
};

// ---- 捕获统计 ----

struct CaptureStats {
    uint32_t packets_captured;   // 成功捕获
    uint32_t packets_dropped;    // 环形缓冲满丢弃
    uint32_t packets_filtered;   // 被 BPF 过滤掉
    uint32_t bytes_captured;
    uint32_t peak_ring_usage;    // 环形缓冲历史最高占用量
};

// ---- 包缓冲区 ----

struct PacketBuffer {
    uint32_t length;
    uint32_t timestamp_sec;
    uint32_t timestamp_usec;
    uint8_t  data[1514];         // 最大以太网帧
};

// ---- PacketCapture VNode (挂载到 /dev/pcap0) ----

class PacketCapture : public VNode {
public:
    static PacketCapture& instance() {
        static PacketCapture pcap;
        return pcap;
    }

    void init();

    // ethernetif.cpp RX/TX 钩子
    void tap_rx_packet(const uint8_t* buffer, int len);
    void tap_tx_packet(const uint8_t* buffer, int len);

    // 过滤器
    void set_filter(const BpfFilter& filter);
    const BpfFilter& get_filter() const { return filter_; }

    // 统计
    CaptureStats get_stats() const;
    void         reset_stats();

    // VNode 接口 — /dev/pcap0
    int  open_file(const char* path, int flags, void** priv) override;
    int  close_file(void* priv) override;
    int  read(char* buf, int len, int offset, void* priv) override;
    int  write(const char* buf, int len, int offset, void* priv) override;
    int  ioctl(int request, void* arg, void* priv) override;

    // ioctl 命令码
    static constexpr int IOCTL_SET_FILTER      = 1;
    static constexpr int IOCTL_ENABLE_PROMISC  = 2;
    static constexpr int IOCTL_DISABLE_PROMISC = 3;
    static constexpr int IOCTL_GET_STATS       = 4;
    static constexpr int IOCTL_RESET_STATS     = 5;
    static constexpr int IOCTL_GET_FILTER      = 6;

private:
    PacketCapture() = default;

    static constexpr int RING_SIZE        = 64;
    static constexpr int MAX_FRAME        = 1514;
    static constexpr int SNAPLEN_DEFAULT  = 65535;

    // 内部过滤器匹配
    bool match_filter_l2_(const uint8_t* buf, int len);
    bool match_filter_l3_(const uint8_t* buf, int len);
    bool match_filter_l4_(const uint8_t* buf, int len, uint8_t ip_proto, int ip_hdr_len);

    // 时间戳辅助
    static uint32_t get_wall_clock_ms_();
    static void     fill_timestamp_(PacketBuffer* pbuf);

    // PCAP 文件格式头
    struct __attribute__((packed)) pcap_file_hdr_t {
        uint32_t magic_number;    // 0xa1b2c3d4 (native byte order: little-endian)
        uint16_t version_major;   // 2
        uint16_t version_minor;   // 4
        int32_t  thiszone;        // GMT 偏移 (0=UTC)
        uint32_t sigfigs;         // 时间戳精度 (0)
        uint32_t snaplen;         // 快照长度
        uint32_t network;         // 链路类型 (1=Ethernet)
    };

    // PCAP 数据包记录头
    struct __attribute__((packed)) pcap_rec_hdr_t {
        uint32_t ts_sec;          // 秒
        uint32_t ts_usec;         // 微秒
        uint32_t incl_len;        // 实际捕获字节数
        uint32_t orig_len;        // 原始长度
    };

    // ---- 成员 ----

    MemoryPool<PacketBuffer, RING_SIZE> pool_;
    PacketBuffer*  ring_[RING_SIZE];
    int            head_ = 0;       // 写入位置
    int            tail_ = 0;       // 读取位置
    mutable Mutex  ring_lock_;

    BpfFilter      filter_;
    mutable Mutex  filter_lock_;

    CaptureStats   stats_;
    mutable Mutex  stats_lock_;

    bool           is_open_ = false;
};

#endif // PACKET_CAPTURE_HPP
