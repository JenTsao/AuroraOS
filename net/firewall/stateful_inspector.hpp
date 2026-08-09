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

    bool process_tcp_packet(const uint8_t* packet, int len);
    void tick();

private:
    TcpConnection connections_[MAX_CONNECTIONS]{};
};

#endif // STATEFUL_INSPECTOR_HPP
