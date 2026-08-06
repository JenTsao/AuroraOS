#ifndef FIREWALL_ENGINE_HPP
#define FIREWALL_ENGINE_HPP

#include "rule_table.hpp"
#include "stateful_inspector.hpp"
#include "traffic_shaper.hpp"
#include "../../kernel/security_monitor.hpp"

class FirewallEngine {
public:
    static FirewallEngine& instance() {
        static FirewallEngine engine;
        return engine;
    }

    bool process_packet(const uint8_t* packet, int len, const char* interface) {
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

    void tick() {
        if (!enabled_) return;
        stateful_inspector_.tick();
        traffic_shaper_.tick();
    }

    void enable(bool enable) { enabled_ = enable; }
    bool is_enabled() const { return enabled_; }

    RuleTable& get_rule_table() { return rule_table_; }

private:
    FirewallEngine() = default;

    RuleTable rule_table_;
    StatefulInspector stateful_inspector_;
    TrafficShaper traffic_shaper_;
    
    bool enabled_ = true;
};

#endif // FIREWALL_ENGINE_HPP
