// =============================================================================
// tests/fuzzing/network_fuzzer.cpp — 防火墙数据包 + 规则命令模糊测试 harness
//
// 靶子（均为解析不可信输入的真实攻击面）：
//   1. FirewallEngine::process_packet() — 解析以太网/IPv4/ICMP/UDP/TCP 包头，
//      走 StealthIdentity 探测抑制、TrafficShaper DDoS 阈值、RuleTable 匹配、
//      StatefulInspector TCP 状态机（net/firewall/firewall_engine.cpp）。
//   2. RuleParser::parse_command() — 解析 "fw add src_ip ... dst_port ..." 等
//      shell 命令字符串（net/firewall/rule_parser.cpp），含手写 strtok/数值解析。
//
// 修复说明：原文件调用 net::firewall::FirewallEngine::inspect_packet(...,
// Direction::IN)，该命名空间/类/方法/枚举均不存在（真实 API 是全局命名空间的
// FirewallEngine::process_packet(const uint8_t*, int, const char*)）。因原文件
// 从未被任何 CMake target 引用，这个编译错误从未暴露。本文件用真实 API 重写。
//
// 由 fuzzer_main.cpp 的 LLVMFuzzerTestOneInput 统一 dispatch 调用。
// =============================================================================

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../net/firewall/firewall_engine.hpp"
#include "../../net/firewall/rule_parser.hpp"

extern void aurora_fuzz_net(const uint8_t* data, size_t size) {
    if (size == 0)
        return;

    // 用首字节的最高位选择子靶子，其余字节作为负载。
    const uint8_t mode = data[0];
    const uint8_t* payload = data + 1;
    size_t payload_len = size - 1;

    if (mode & 0x80u) {
        // ── 靶子 2：规则命令解析（需 NUL 结尾 C 字符串）──
        char cmd[256];
        const size_t n = payload_len < 255u ? payload_len : 255u;
        if (n > 0)
            memcpy(cmd, payload, n);
        cmd[n] = '\0';
        RuleParser::parse_command(cmd);
    } else {
        // ── 靶子 1：数据包检测（限制到 MTU，避免巨型分配）──
        if (payload_len > 1500u)
            payload_len = 1500u;
        FirewallEngine::instance().process_packet(payload, static_cast<int>(payload_len), "eth0");
    }

    // 驱动状态机老化，覆盖 StatefulInspector::tick 路径。
    FirewallEngine::instance().tick();
}
