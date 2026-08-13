#ifndef FIREWALL_RULE_TABLE_HPP
#define FIREWALL_RULE_TABLE_HPP

#include <stdint.h>
#include <string.h>

enum class FwAction {
    ACCEPT,
    DROP,
    REJECT
};

// Simplified firewall rule
struct FwRule {
    bool enabled = false;

    // IP filter
    bool match_src_ip = false;
    uint32_t src_ip = 0;

    bool match_dst_ip = false;
    uint32_t dst_ip = 0;

    // Port filter
    bool match_src_port = false;
    uint16_t src_port = 0;

    bool match_dst_port = false;
    uint16_t dst_port = 0;

    // Protocol filter
    bool match_protocol = false;
    uint8_t protocol = 0;

    // TCP Flags filter
    bool match_tcp_flags = false;
    uint8_t tcp_flags_mask = 0;
    uint8_t tcp_flags_value = 0;

    // Interface filter
    bool match_interface = false;
    char interface[8] = {0};

    FwAction action = FwAction::ACCEPT;
};

class RuleTable {
public:
    static constexpr int MAX_RULES = 16;

    bool add_rule(const FwRule& rule);
    bool delete_rule(int index);
    bool enable_rule(int index, bool enable);
    const FwRule* get_rules() const;
    FwAction match(const uint8_t* packet, int len, const char* interface) const;

private:
    FwRule rules_[MAX_RULES];
};

#endif // FIREWALL_RULE_TABLE_HPP
