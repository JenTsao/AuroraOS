#ifndef RULE_PARSER_HPP
#define RULE_PARSER_HPP

#include "firewall_engine.hpp"
#include "../../syscall/syscall.hpp" // For sys_print or similar logging
#include <string.h>

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
            int index = (int)parse_uint(args + 7);
            if (FirewallEngine::instance().get_rule_table().delete_rule(index)) {
                sys_print("Rule deleted.\r\n");
            } else {
                sys_print("Failed to delete rule.\r\n");
            }
        } else if (strncmp(args, "add ", 4) == 0) {
            FwRule new_rule;
            const char* ptr = args + 4;

            // --- Parse action ---
            if (strstr(ptr, "action DROP")) {
                new_rule.action = FwAction::DROP;
            } else if (strstr(ptr, "action REJECT")) {
                new_rule.action = FwAction::REJECT;
            } else {
                new_rule.action = FwAction::ACCEPT;
            }

            // --- Parse protocol ---
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

            // --- Parse src_ip (hex: 0xC0A80101 or dotted: 192.168.1.1) ---
            {
                const char* key = strstr(ptr, "src_ip ");
                if (key) {
                    new_rule.match_src_ip = true;
                    new_rule.src_ip = parse_ip(key + 7);
                }
            }

            // --- Parse dst_ip ---
            {
                const char* key = strstr(ptr, "dst_ip ");
                if (key) {
                    new_rule.match_dst_ip = true;
                    new_rule.dst_ip = parse_ip(key + 7);
                }
            }

            // --- Parse src_port ---
            {
                const char* key = strstr(ptr, "src_port ");
                if (key) {
                    new_rule.match_src_port = true;
                    new_rule.src_port = (uint16_t)parse_uint(key + 9);
                }
            }

            // --- Parse dst_port ---
            {
                const char* key = strstr(ptr, "dst_port ");
                if (key) {
                    new_rule.match_dst_port = true;
                    new_rule.dst_port = (uint16_t)parse_uint(key + 9);
                }
            }

            // --- Validation: require at least one match predicate ---
            // Without this check, a rule like "fw add action DROP" would match
            // every IPv4 packet and blackhole all traffic.
            bool has_predicate = new_rule.match_src_ip || new_rule.match_dst_ip ||
                                 new_rule.match_src_port || new_rule.match_dst_port ||
                                 new_rule.match_protocol;
            if (!has_predicate) {
                sys_print("Error: rule requires at least one match predicate (src_ip, dst_ip, src_port, dst_port, protocol).\r\n");
                return;
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

private:
    // Self-contained unsigned integer parser (replaces atoi/strtoul so the
    // header does not depend on <cstdlib>, which some freestanding/newlib-nano
    // toolchains do not expose in the global namespace).
    // Supports optional "0x" prefix (hex) and plain decimal.
    static uint32_t parse_uint(const char* str) {
        while (*str == ' ' || *str == '\t') str++;
        uint32_t base = 10;
        if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
            base = 16;
            str += 2;
        }
        uint32_t result = 0;
        while (*str) {
            char c = *str++;
            uint32_t digit;
            if (c >= '0' && c <= '9') digit = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') digit = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = (uint32_t)(c - 'A' + 10);
            else break; // stop at non-digit (space, '.', '\r', etc.)
            if (digit >= base) break;
            result = result * base + digit;
        }
        return result;
    }

    // Parse an IPv4 address string into a 32-bit integer.
    // Supports:
    //   0xC0A80101  (hex literal)
    //   192.168.1.1 (dotted decimal)
    static uint32_t parse_ip(const char* str) {
        while (*str == ' ') str++;

        // Hex format (0x...)
        if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
            return parse_uint(str);
        }

        // Dotted decimal: count dots to detect the format
        int dots = 0;
        for (const char* c = str; *c; c++) {
            if (*c == '.') dots++;
        }

        if (dots == 3) {
            uint8_t octets[4];
            const char* s = str;
            for (int i = 0; i < 4; i++) {
                octets[i] = (uint8_t)parse_uint(s);
                // advance past the next dot (if any) before parsing the next octet
                while (*s && *s != '.') s++;
                if (*s == '.' && i < 3) s++;
                else if (*s != '.' && i < 3) return 0; // malformed
            }
            return ((uint32_t)octets[0] << 24) | ((uint32_t)octets[1] << 16) |
                   ((uint32_t)octets[2] << 8)  | octets[3];
        }

        // Plain decimal (unusual, but treat as 32-bit host byte order)
        return parse_uint(str);
    }
};

#endif // RULE_PARSER_HPP
