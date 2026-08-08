#ifndef FIREWALL_ENGINE_HPP
#define FIREWALL_ENGINE_HPP

#include "rule_table.hpp"
#include "stateful_inspector.hpp"
#include "traffic_shaper.hpp"
#include "../../kernel/security_monitor.hpp"

class FirewallEngine {
public:
    static FirewallEngine& instance();

    bool process_packet(const uint8_t* packet, int len, const char* interface);
    void tick();

    void enable(bool enable);
    bool is_enabled() const;

    RuleTable& get_rule_table() { return rule_table_; }

private:
    FirewallEngine() = default;

    RuleTable rule_table_;
    StatefulInspector stateful_inspector_;
    TrafficShaper traffic_shaper_;
    
    bool enabled_ = true;
};

#endif // FIREWALL_ENGINE_HPP
