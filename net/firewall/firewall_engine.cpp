#include "../../services/firewall/firewall_audit.hpp"
#include "firewall_engine.hpp"
#include "../stealth_identity.hpp"

FirewallEngine& FirewallEngine::instance() {
    static FirewallEngine engine;
    return engine;
}

bool FirewallEngine::process_packet(const uint8_t* packet, int len, const char* interface) {
    if (!enabled_)
        return true; // Let packet pass if firewall is disabled

    // 0. Stealth Identity & Inbound Probe Suppression
    if (len >= 34) { // Ethernet (14) + IP Header (min 20)
        uint16_t ethertype = (static_cast<uint16_t>(packet[12]) << 8) | packet[13];
        if (ethertype == 0x0800) { // IPv4
            uint8_t ip_proto = packet[23];
            uint8_t ihl = packet[14] & 0x0F;
            int ip_hdr_len = ihl * 4;
            if (ihl >= 5 && len >= 14 + ip_hdr_len) {
                if (ip_proto == 1 /* ICMP */ && len >= 14 + ip_hdr_len + 1) {
                    uint8_t icmp_type = packet[14 + ip_hdr_len];
                    if (StealthIdentity::instance().should_drop_inbound_probe(1, 0, icmp_type)) {
                        return false;
                    }
                } else if (ip_proto == 17 /* UDP */ && len >= 14 + ip_hdr_len + 4) {
                    uint16_t dport = (static_cast<uint16_t>(packet[14 + ip_hdr_len + 2]) << 8) |
                                     packet[14 + ip_hdr_len + 3];
                    if (StealthIdentity::instance().should_drop_inbound_probe(17, dport)) {
                        return false;
                    }
                }
            }
        }
    }

    // 1. Threshold Protection & Traffic Shaping (DDoS mitigation)
    if (!traffic_shaper_.process_packet(packet, len)) {
        return false; // Dropped by Traffic Shaper
    }

    // 2. Rule Table Matching
    FwAction action = rule_table_.match(packet, len, interface);
    if (action == FwAction::DROP || action == FwAction::REJECT) {
        auroraos::firewall::FirewallAudit::instance().log_drop("Rule Drop", packet, len);
        return false;
    }

    // 3. Stateful Inspection
    if (!stateful_inspector_.process_tcp_packet(packet, len)) {
        auroraos::firewall::FirewallAudit::instance().log_drop("Stateful Drop", packet, len);
        return false;
    }

    return true;
}

void FirewallEngine::tick() {
    if (!enabled_)
        return;
    stateful_inspector_.tick();
    traffic_shaper_.tick();
}

void FirewallEngine::enable(bool enable) {
    enabled_ = enable;
}

bool FirewallEngine::is_enabled() const {
    return enabled_;
}
