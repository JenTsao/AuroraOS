#include "packet_capture.hpp"
#include "protocol_analyzer.hpp"
#include "eth_driver.hpp"
#include <string.h>


void PacketCapture::init() {
    is_open_ = false;
    head_ = 0;
    tail_ = 0;
    VfsManager::instance().mount("/dev/pcap0", this);
}

void PacketCapture::tap_rx_packet(const uint8_t* buffer, int len) {
    if (!is_open_) return;
    
    // Check BPF filter
    {
        LockGuard lock(filter_mutex_);
        if (!ProtocolAnalyzer::match_filter(buffer, len, filter_)) {
            return;
        }
    }

    // Allocate from memory pool
    PacketBuffer* pbuf = packet_pool_.allocate();
    if (!pbuf) return; // Dropped

    pbuf->length = len > 1514 ? 1514 : len;
    
    // Note: in a real system we'd get a proper timestamp. Using 0 for simplicity or fake ticks.
    pbuf->timestamp_sec = 0; 
    pbuf->timestamp_usec = 0;
    
    memcpy(pbuf->data, buffer, pbuf->length);

    // Push to ring buffer
    {
        LockGuard lock(ring_mutex_);
        int next_head = (head_ + 1) % 64;
        if (next_head == tail_) {
            // Buffer full, drop packet
            packet_pool_.deallocate(pbuf);
            return;
        }
        capture_ring_[head_] = pbuf;
        head_ = next_head;
    }
}

void PacketCapture::tap_tx_packet(const uint8_t* buffer, int len) {
    // Similar to rx if we want to capture tx
    tap_rx_packet(buffer, len);
}

void PacketCapture::set_filter(const BpfFilter& filter) {
    LockGuard lock(filter_mutex_);
    filter_ = filter;
}

int PacketCapture::open_file(const char* path, int flags, void** priv) {
    if (is_open_) return -1; // Only one reader allowed
    
    is_open_ = true;
    head_ = 0;
    tail_ = 0;
    
    // Return a fake fd state if needed, but we can just use the VNode state
    return 0;
}

int PacketCapture::close_file(void* priv) {
    is_open_ = false;
    
    // Clear ring buffer
    LockGuard lock(ring_mutex_);
    while (tail_ != head_) {
        packet_pool_.deallocate(capture_ring_[tail_]);
        tail_ = (tail_ + 1) % 64;
    }
    
    // Disable promiscuous mode if it was enabled
    StellarisEth::instance().set_promiscuous_mode(false);
    return 0;
}

int PacketCapture::read(char* buf, int len, int offset, void* priv) {
    if (!is_open_ || len <= 0) return 0;
    
    if (offset == 0 && len >= sizeof(pcap_hdr_s)) {
        // Output pcap global header
        pcap_hdr_s hdr;
        hdr.magic_number = 0xa1b2c3d4;
        hdr.version_major = 2;
        hdr.version_minor = 4;
        hdr.thiszone = 0;
        hdr.sigfigs = 0;
        hdr.snaplen = 65535;
        hdr.network = 1; // Ethernet
        
        memcpy(buf, &hdr, sizeof(hdr));
        return sizeof(hdr);
    }
    
    // Wait for packet or return 0 (non-blocking for simplicity)
    PacketBuffer* pbuf = nullptr;
    {
        LockGuard lock(ring_mutex_);
        if (head_ != tail_) {
            pbuf = capture_ring_[tail_];
            tail_ = (tail_ + 1) % 64;
        }
    }
    
    if (!pbuf) return 0; // No packets available
    
    int packet_size = pbuf->length;
    int total_size = sizeof(pcaprec_hdr_s) + packet_size;
    
    if (len < total_size) {
        // Buffer too small, drop it for now
        packet_pool_.deallocate(pbuf);
        return 0;
    }
    
    pcaprec_hdr_s phdr;
    phdr.ts_sec = pbuf->timestamp_sec;
    phdr.ts_usec = pbuf->timestamp_usec;
    phdr.incl_len = packet_size;
    phdr.orig_len = packet_size;
    
    memcpy(buf, &phdr, sizeof(phdr));
    memcpy(buf + sizeof(phdr), pbuf->data, packet_size);
    
    packet_pool_.deallocate(pbuf);
    return total_size;
}

int PacketCapture::write(const char* buf, int len, int offset, void* priv) {
    return -1; // Read-only
}

int PacketCapture::ioctl(int request, void* arg, void* priv) {
    switch (request) {
        case IOCTL_SET_FILTER:
            if (arg) {
                set_filter(*reinterpret_cast<BpfFilter*>(arg));
                return 0;
            }
            break;
        case IOCTL_ENABLE_PROMISC:
            StellarisEth::instance().set_promiscuous_mode(true);
            return 0;
        case IOCTL_DISABLE_PROMISC:
            StellarisEth::instance().set_promiscuous_mode(false);
            return 0;
    }
    return -1;
}
