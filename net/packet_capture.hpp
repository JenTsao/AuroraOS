#ifndef PACKET_CAPTURE_HPP
#define PACKET_CAPTURE_HPP

#include <stdint.h>
#include <stddef.h>
#include "../kernel/memory_pool.hpp"
#include "../kernel/mutex.hpp"
#include "../vfs/vfs.hpp"

// Forward declaration for protocol analyzer
class ProtocolAnalyzer;

// Packet buffer structure for zero-copy capture
struct PacketBuffer {
    uint32_t length;
    uint32_t timestamp_sec;
    uint32_t timestamp_usec;
    uint8_t data[1514]; // Max Ethernet frame size
};

// BPF-style filter rules
struct BpfFilter {
    bool enable_mac_filter = false;
    uint8_t target_mac[6] = {0};

    bool enable_ip_filter = false;
    uint32_t target_ip = 0; // Network byte order

    bool enable_port_filter = false;
    uint16_t target_port = 0; // Network byte order

    bool enable_protocol_filter = false;
    uint8_t target_protocol = 0; // e.g., IPPROTO_TCP, IPPROTO_UDP
};

class PacketCapture : public VNode {
public:
    static PacketCapture& instance() {
        static PacketCapture pcap;
        return pcap;
    }

    void init();
    
    // Call this from ethernetif.cpp rx path
    void tap_rx_packet(const uint8_t* buffer, int len);
    
    // Call this from ethernetif.cpp tx path (optional)
    void tap_tx_packet(const uint8_t* buffer, int len);

    // Set filter rules
    void set_filter(const BpfFilter& filter);

    // VNode implementation for /dev/pcap0
    int open_file(const char* path, int flags, void** priv) override;
    int close_file(void* priv) override;
    int read(char* buf, int len, int offset, void* priv) override;
    int write(const char* buf, int len, int offset, void* priv) override;
    int ioctl(int request, void* arg, void* priv) override;

    // Constants for ioctl
    static constexpr int IOCTL_SET_FILTER = 1;
    static constexpr int IOCTL_ENABLE_PROMISC = 2;
    static constexpr int IOCTL_DISABLE_PROMISC = 3;

private:
    PacketCapture() = default;
    
    void process_packet(const uint8_t* buffer, int len);
    bool match_filter(const uint8_t* buffer, int len);

    MemoryPool<PacketBuffer, 64> packet_pool_;
    
    // Simple ring buffer for captured packets
    PacketBuffer* capture_ring_[64];
    int head_ = 0;
    int tail_ = 0;
    Mutex ring_mutex_;
    
    BpfFilter filter_;
    Mutex filter_mutex_;
    
    bool is_open_ = false;
    
    // PCAP file header
    struct pcap_hdr_s {
        uint32_t magic_number;   /* magic number */
        uint16_t version_major;  /* major version number */
        uint16_t version_minor;  /* minor version number */
        int32_t  thiszone;       /* GMT to local correction */
        uint32_t sigfigs;        /* accuracy of timestamps */
        uint32_t snaplen;        /* max length of captured packets, in octets */
        uint32_t network;        /* data link type */
    };

    // PCAP packet header
    struct pcaprec_hdr_s {
        uint32_t ts_sec;         /* timestamp seconds */
        uint32_t ts_usec;        /* timestamp microseconds */
        uint32_t incl_len;       /* number of octets of packet saved in file */
        uint32_t orig_len;       /* actual length of packet */
    };
};

#endif // PACKET_CAPTURE_HPP
