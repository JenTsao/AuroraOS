#include "rule_table.hpp"

bool RuleTable::add_rule(const FwRule& rule) {
    for (int i = 0; i < MAX_RULES; i++) {
        if (!rules_[i].enabled) {
            rules_[i] = rule;
            rules_[i].enabled = true;
            return true;
        }
    }
    return false;
}

bool RuleTable::delete_rule(int index) {
    if (index >= 0 && index < MAX_RULES) {
        rules_[index].enabled = false;
        return true;
    }
    return false;
}

bool RuleTable::enable_rule(int index, bool enable) {
    if (index >= 0 && index < MAX_RULES) {
        rules_[index].enabled = enable;
        return true;
    }
    return false;
}

const FwRule* RuleTable::get_rules() const {
    return rules_;
}

FwAction RuleTable::match(const uint8_t* packet, int len, const char* interface) const {
    if (len < 14) return FwAction::ACCEPT;
    
    uint16_t eth_type = (packet[12] << 8) | packet[13];
    if (eth_type != 0x0800) return FwAction::ACCEPT; // Only process IPv4 for now
    if (len < 34) return FwAction::ACCEPT;
    
    uint32_t src_ip = (packet[26] << 24) | (packet[27] << 16) | (packet[28] << 8) | packet[29];
    uint32_t dst_ip = (packet[30] << 24) | (packet[31] << 16) | (packet[32] << 8) | packet[33];
    uint8_t protocol = packet[23];
    
    uint8_t ihl = packet[14] & 0x0F;
    int ip_header_len = ihl * 4;
    
    uint16_t src_port = 0, dst_port = 0;
    uint8_t tcp_flags = 0;
    
    if (protocol == 6 || protocol == 17) {
        if (len >= 14 + ip_header_len + 4) {
            src_port = (packet[14 + ip_header_len] << 8) | packet[14 + ip_header_len + 1];
            dst_port = (packet[14 + ip_header_len + 2] << 8) | packet[14 + ip_header_len + 3];
        }
        if (protocol == 6 && len >= 14 + ip_header_len + 14) {
            tcp_flags = packet[14 + ip_header_len + 13];
        }
    }

    // First matching rule decides action (like iptables)
    for (int i = 0; i < MAX_RULES; i++) {
        const FwRule& r = rules_[i];
        if (!r.enabled) continue;
        
        if (r.match_interface && strncmp(r.interface, interface, sizeof(r.interface)) != 0) continue;
        if (r.match_src_ip && r.src_ip != src_ip) continue;
        if (r.match_dst_ip && r.dst_ip != dst_ip) continue;
        if (r.match_protocol && r.protocol != protocol) continue;
        if (r.match_src_port && r.src_port != src_port) continue;
        if (r.match_dst_port && r.dst_port != dst_port) continue;
        if (r.match_tcp_flags && protocol == 6) {
            if ((tcp_flags & r.tcp_flags_mask) != r.tcp_flags_value) continue;
        }
        
        return r.action;
    }
    
    return FwAction::ACCEPT; // Default policy
}
