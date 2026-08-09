#include "firewall_engine.hpp"

FirewallEngine& FirewallEngine::instance() {
    static FirewallEngine engine;
    return engine;
}

bool FirewallEngine::process_packet(const uint8_t* packet, int len, const char* interface) {
    if (!enabled_) return true; // Let packet pass if firewall is disabled
    
    // 1. Threshold Protection & Traffic Shaping (DDoS mitigation)
    if (!traffic_shaper_.process_packet(packet, len)) {
        return false; // Dropped by Traffic Shaper
    }

    // 2. Rule Table Matching
    FwAction action = rule_table_.match(packet, len, interface);
    if (action == FwAction::DROP || action == FwAction::REJECT) {
        SecurityMonitor::instance().report_firewall_anomaly("Rule Drop");
        return false;
    }

    // 3. Stateful Inspection
    if (!stateful_inspector_.process_tcp_packet(packet, len)) {
        SecurityMonitor::instance().report_firewall_anomaly("Stateful Drop");
        return false;
    }

    return true;
}

void FirewallEngine::tick() {
    if (!enabled_) return;
    stateful_inspector_.tick();
    traffic_shaper_.tick();
}

void FirewallEngine::enable(bool enable) {
    enabled_ = enable;
}

bool FirewallEngine::is_enabled() const {
    return enabled_;
}
