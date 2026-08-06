#ifndef RULE_PARSER_HPP
#define RULE_PARSER_HPP

#include "firewall_engine.hpp"
#include "../../kernel/syscall.hpp" // For sys_print or similar logging
#include <string.h>
#include <stdlib.h>

class RuleParser {
public:
    // Simple parser for commands like:
    // fw enable
    // fw disable
    // fw list
    // fw add src_ip 192.168.1.1 dst_port 80 action DROP
    // fw delete <index>
    static void parse_command(const char* cmd) {
        if (strncmp(cmd, "fw ", 3) != 0) return;
        
        const char* args = cmd + 3;
        
        if (strncmp(args, "enable", 6) == 0) {
            FirewallEngine::instance().enable(true);
            sys_print("Firewall enabled.\r\n");
        } else if (strncmp(args, "disable", 7) == 0) {
            FirewallEngine::instance().enable(false);
            sys_print("Firewall disabled.\r\n");
        } else if (strncmp(args, "list", 4) == 0) {
            sys_print("Firewall rules:\r\n");
            const FwRule* rules = FirewallEngine::instance().get_rule_table().get_rules();
            for (int i = 0; i < RuleTable::MAX_RULES; i++) {
                if (rules[i].enabled) {
                    // In a real system we'd format the rule printout
                    sys_print("  Rule ");
                    // Using basic output for now
                    // ... (print rule details)
                    sys_print(" (active)\r\n");
                }
            }
        } else if (strncmp(args, "delete ", 7) == 0) {
            int index = atoi(args + 7);
            if (FirewallEngine::instance().get_rule_table().delete_rule(index)) {
                sys_print("Rule deleted.\r\n");
            } else {
                sys_print("Failed to delete rule.\r\n");
            }
        } else if (strncmp(args, "add ", 4) == 0) {
            FwRule new_rule;
            // Simple space-separated parsing
            // fw add src_ip 0x... action DROP
            // For production, we'd use strtok and parse IPs, ports properly.
            // Example stub parsing:
            const char* ptr = args + 4;
            if (strstr(ptr, "action DROP")) {
                new_rule.action = FwAction::DROP;
            } else if (strstr(ptr, "action REJECT")) {
                new_rule.action = FwAction::REJECT;
            } else {
                new_rule.action = FwAction::ACCEPT;
            }
            
            if (strstr(ptr, "protocol TCP")) {
                new_rule.match_protocol = true;
                new_rule.protocol = 6;
            } else if (strstr(ptr, "protocol UDP")) {
                new_rule.match_protocol = true;
                new_rule.protocol = 17;
            } else if (strstr(ptr, "protocol ICMP")) {
                new_rule.match_protocol = true;
                new_rule.protocol = 1;
            }
            
            if (FirewallEngine::instance().get_rule_table().add_rule(new_rule)) {
                sys_print("Rule added.\r\n");
            } else {
                sys_print("Rule table full.\r\n");
            }
        } else {
            sys_print("Unknown fw command.\r\n");
        }
    }
};

#endif // RULE_PARSER_HPP
