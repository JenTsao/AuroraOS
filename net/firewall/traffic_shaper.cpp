#include "traffic_shaper.hpp"

bool TrafficShaper::process_packet(const uint8_t* packet, int len) {
    if (len < 34)
        return true;

    uint16_t eth_type = (packet[12] << 8) | packet[13];
    if (eth_type != 0x0800)
        return true;

    uint32_t src_ip = (packet[26] << 24) | (packet[27] << 16) | (packet[28] << 8) | packet[29];
    uint8_t protocol = packet[23];

    extern volatile uint32_t tick_count;
    uint32_t now = tick_count;

    HostStats* stats = get_or_create_stats(src_ip, now);
    if (!stats)
        return true; // No space, let it pass or drop? Let pass to be safe.

    if (protocol == 1) { // ICMP
        stats->icmp_count++;
        if (stats->icmp_count > ICMP_LIMIT) {
            SecurityMonitor::instance().report_firewall_anomaly("ICMP Flood");
            return false;
        }
    } else if (protocol == 6) { // TCP
        uint8_t ihl = packet[14] & 0x0F;
        int ip_header_len = ihl * 4;
        if (len >= 14 + ip_header_len + 14) {
            uint16_t dst_port = (packet[14 + ip_header_len + 2] << 8) | packet[14 + ip_header_len + 3];
            uint8_t flags = packet[14 + ip_header_len + 13];

            if (flags & 0x02) { // SYN
                stats->syn_count++;
                if (stats->syn_count > SYN_LIMIT) {
                    SecurityMonitor::instance().report_firewall_anomaly("SYN Flood");
                    return false;
                }

                if (stats->last_dst_port != dst_port) {
                    stats->port_scan_count++;
                    stats->last_dst_port = dst_port;
                    if (stats->port_scan_count > PORT_SCAN_LIMIT) {
                        SecurityMonitor::instance().report_firewall_anomaly("Port Scan");
                        return false;
                    }
                }
            }
        }
    }

    return true;
}

void TrafficShaper::tick() {
    extern volatile uint32_t tick_count;
    uint32_t now = tick_count;

    for (int i = 0; i < MAX_HOSTS; i++) {
        if (hosts_[i].active) {
            if (now - hosts_[i].last_activity_tick > RESET_INTERVAL) {
                hosts_[i].active = false;
            }
        }
    }
}

HostStats* TrafficShaper::get_or_create_stats(uint32_t src_ip, uint32_t now) {
    for (int i = 0; i < MAX_HOSTS; i++) {
        if (hosts_[i].active && hosts_[i].src_ip == src_ip) {
            hosts_[i].last_activity_tick = now;
            return &hosts_[i];
        }
    }
    for (int i = 0; i < MAX_HOSTS; i++) {
        if (!hosts_[i].active) {
            hosts_[i].active = true;
            hosts_[i].src_ip = src_ip;
            hosts_[i].syn_count = 0;
            hosts_[i].icmp_count = 0;
            hosts_[i].port_scan_count = 0;
            hosts_[i].last_dst_port = 0;
            hosts_[i].last_activity_tick = now;
            return &hosts_[i];
        }
    }
    return nullptr;
}
