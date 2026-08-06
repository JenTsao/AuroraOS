#ifndef PROTOCOL_ANALYZER_HPP
#define PROTOCOL_ANALYZER_HPP

#include <stdint.h>
#include "packet_capture.hpp"

class ProtocolAnalyzer {
public:
    static bool match_filter(const uint8_t* packet, int len, const BpfFilter& filter) {
        if (len < 14) {
            // Cannot parse Ethernet header
            return (!filter.enable_mac_filter && !filter.enable_ip_filter && !filter.enable_port_filter && !filter.enable_protocol_filter);
        }

        if (filter.enable_mac_filter) {
            bool mac_match = true;
            for (int i = 0; i < 6; i++) {
                if (packet[i] != filter.target_mac[i]) {
                    mac_match = false;
                    break;
                }
            }
            if (!mac_match) return false;
        }

        uint16_t eth_type = (packet[12] << 8) | packet[13];
        if (eth_type != 0x0800) { // Not IPv4
            if (filter.enable_ip_filter || filter.enable_port_filter || filter.enable_protocol_filter) {
                return false;
            }
            return true;
        }

        if (len < 34) return false; // Incomplete IPv4 header

        if (filter.enable_ip_filter) {
            // Offset 30 is Dest IP in Ethernet + IPv4
            const uint8_t* target_ip_ptr = reinterpret_cast<const uint8_t*>(&filter.target_ip);
            if (packet[30] != target_ip_ptr[0] || packet[31] != target_ip_ptr[1] || 
                packet[32] != target_ip_ptr[2] || packet[33] != target_ip_ptr[3]) {
                return false;
            }
        }

        uint8_t ip_proto = packet[23];
        if (filter.enable_protocol_filter) {
            if (ip_proto != filter.target_protocol) return false;
        }

        if (filter.enable_port_filter) {
            uint8_t ihl = packet[14] & 0x0F;
            int ip_header_len = ihl * 4;
            if (len < 14 + ip_header_len + 4) return false; // Incomplete L4 header

            if (ip_proto == 6 || ip_proto == 17) { // TCP or UDP
                uint16_t src_port = (packet[14 + ip_header_len + 0] << 8) | packet[14 + ip_header_len + 1];
                uint16_t dst_port = (packet[14 + ip_header_len + 2] << 8) | packet[14 + ip_header_len + 3];
                
                // Assuming target_port is network byte order (Big Endian)
                uint16_t target_port_host = (filter.target_port >> 8) | (filter.target_port << 8);
                if (src_port != target_port_host && dst_port != target_port_host) {
                    return false;
                }
            } else {
                return false; // Not TCP/UDP but port filter is enabled
            }
        }

        return true;
    }
};

#endif // PROTOCOL_ANALYZER_HPP
