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
    static void parse_command(const char* cmd);

private:
    static uint32_t parse_uint(const char* str);
    static uint32_t parse_ip(const char* str);
};

#endif // RULE_PARSER_HPP
