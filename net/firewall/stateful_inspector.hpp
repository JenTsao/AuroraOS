#ifndef STATEFUL_INSPECTOR_HPP
#define STATEFUL_INSPECTOR_HPP

#include <stdint.h>

// Simplified TCP States
enum class TcpState {
    CLOSED,
    SYN_SENT,
    SYN_RECEIVED,
    ESTABLISHED,
    FIN_WAIT
};

struct TcpConnection {
    bool active = false;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    TcpState state;
    uint32_t last_activity_tick;
};

class StatefulInspector {
public:
    static constexpr int MAX_CONNECTIONS = 256;
    static constexpr uint32_t TIMEOUT_TICKS = 30000; // 30s timeout

    bool process_tcp_packet(const uint8_t* packet, int len) {
        if (len < 34) return true; // Let it pass if we can't inspect
        
        uint16_t eth_type = (packet[12] << 8) | packet[13];
        if (eth_type != 0x0800) return true; // Only IPv4
        
        uint8_t protocol = packet[23];
        if (protocol != 6) return true; // Only TCP
        
        uint8_t ihl = packet[14] & 0x0F;
        // IHL 最小合法值为 5；不做此校验会导致 ip_header_len=0 时从 IP 头区域
        // 读取 TCP 端口和标志位，攻击者可构造 IHL=0 的数据包绕过状态检测。
        if (ihl < 5) return false;
        int ip_header_len = ihl * 4;
        if (len < 14 + ip_header_len + 14) return true;
        
        uint32_t src_ip = (packet[26] << 24) | (packet[27] << 16) | (packet[28] << 8) | packet[29];
        uint32_t dst_ip = (packet[30] << 24) | (packet[31] << 16) | (packet[32] << 8) | packet[33];
        uint16_t src_port = (packet[14 + ip_header_len] << 8) | packet[14 + ip_header_len + 1];
        uint16_t dst_port = (packet[14 + ip_header_len + 2] << 8) | packet[14 + ip_header_len + 3];
        uint8_t flags = packet[14 + ip_header_len + 13];
        
        bool is_syn = (flags & 0x02) != 0;
        bool is_ack = (flags & 0x10) != 0;
        bool is_fin = (flags & 0x01) != 0;
        bool is_rst = (flags & 0x04) != 0;
        
        extern volatile uint32_t tick_count; // From interrupts.cpp
        uint32_t now = tick_count;

        // Find existing connection
        TcpConnection* conn = nullptr;
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (connections_[i].active) {
                if ((connections_[i].src_ip == src_ip && connections_[i].dst_ip == dst_ip &&
                     connections_[i].src_port == src_port && connections_[i].dst_port == dst_port) ||
                    (connections_[i].src_ip == dst_ip && connections_[i].dst_ip == src_ip &&
                     connections_[i].src_port == dst_port && connections_[i].dst_port == src_port)) {
                    conn = &connections_[i];
                    break;
                }
            }
        }

        if (conn) {
            conn->last_activity_tick = now;
            // Update state machine
            if (is_rst) {
                conn->state = TcpState::CLOSED;
                conn->active = false;
            } else if (is_fin) {
                conn->state = TcpState::FIN_WAIT;
            } else if (conn->state == TcpState::SYN_SENT && is_syn && is_ack) {
                conn->state = TcpState::SYN_RECEIVED;
            } else if (conn->state == TcpState::SYN_RECEIVED && is_ack) {
                conn->state = TcpState::ESTABLISHED;
            }
            return true;
        } else {
            // New connection, must start with SYN
            if (is_syn && !is_ack) {
                for (int i = 0; i < MAX_CONNECTIONS; i++) {
                    if (!connections_[i].active) {
                        connections_[i].active = true;
                        connections_[i].src_ip = src_ip;
                        connections_[i].dst_ip = dst_ip;
                        connections_[i].src_port = src_port;
                        connections_[i].dst_port = dst_port;
                        connections_[i].state = TcpState::SYN_SENT;
                        connections_[i].last_activity_tick = now;
                        return true;
                    }
                }
                return false; // Table full, drop
            } else {
                // Out of state packet, drop it
                return false; 
            }
        }
    }

    void tick() {
        extern volatile uint32_t tick_count;
        uint32_t now = tick_count;
        
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (connections_[i].active) {
                if (now - connections_[i].last_activity_tick > TIMEOUT_TICKS) {
                    connections_[i].active = false; // Timeout
                }
            }
        }
    }

private:
    TcpConnection connections_[MAX_CONNECTIONS]{};
};

#endif // STATEFUL_INSPECTOR_HPP
