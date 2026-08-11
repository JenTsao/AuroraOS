#ifndef TRAFFIC_SHAPER_HPP
#define TRAFFIC_SHAPER_HPP

#include <stdint.h>
#include "../../kernel/core/security_monitor.hpp"

struct HostStats {
    bool active = false;
    uint32_t src_ip;
    
    uint32_t syn_count;
    uint32_t icmp_count;
    uint32_t port_scan_count;
    
    uint16_t last_dst_port;
    uint32_t last_activity_tick;
};

class TrafficShaper {
public:
    static constexpr int MAX_HOSTS = 64;
    static constexpr uint32_t SYN_LIMIT = 50; // packets per reset interval
    static constexpr uint32_t ICMP_LIMIT = 20; 
    static constexpr uint32_t PORT_SCAN_LIMIT = 10; // unique ports per interval
    static constexpr uint32_t RESET_INTERVAL = 1000; // 1 second (assuming 1ms tick)

    bool process_packet(const uint8_t* packet, int len);
    void tick();

private:
    HostStats* get_or_create_stats(uint32_t src_ip, uint32_t now);
    HostStats hosts_[MAX_HOSTS]{};
};

#endif // TRAFFIC_SHAPER_HPP
