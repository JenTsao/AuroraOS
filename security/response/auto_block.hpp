// =============================================================================
// security/response/auto_block.hpp
//
// 自动封禁：动态防火墙规则（IP 级封禁，带超时自动解封）
//
//   - block_ip()：将源 IP 注入 FirewallEngine 的 RuleTable（match_src_ip + DROP），
//     并在封禁表记录到期时间
//   - tick()：超时自动解封（删除防火墙规则）
//
// 复用 AuroraOS 特性：FirewallEngine 动态规则表
// 设计原则（遵循 AGENTS.md）：固定数组、零堆分配、noexcept
// =============================================================================
#ifndef AURORA_RESPONSE_AUTO_BLOCK_HPP
#define AURORA_RESPONSE_AUTO_BLOCK_HPP

#include <stdint.h>
#include "../../net/firewall/firewall_engine.hpp"

// 系统 tick 计数器（定义于 boot/interrupts.cpp；宿主测试由 kernel_stubs.cpp 提供）
extern volatile uint32_t tick_count;

namespace aurora {
namespace response {

class AutoBlockManager {
public:
    static constexpr int kMaxBlocks = 16; // 与 RuleTable::MAX_RULES 对齐
    static constexpr uint32_t kDefaultBlockMs = 60000;
    static constexpr uint32_t kPermanent = 0xFFFFFFFFu;

    // 封禁源 IP。duration_ms = 0 表示永久封禁。返回是否已封禁。
    bool block_ip(uint32_t ip, uint32_t duration_ms = kDefaultBlockMs) {
        // 已封禁 → 刷新到期时间
        for (int i = 0; i < kMaxBlocks; ++i) {
            if (entries_[i].active && entries_[i].ip == ip) {
                entries_[i].expire_tick = (duration_ms == 0) ? kPermanent : now_ms_() + duration_ms;
                return true;
            }
        }

        // 新封禁
        for (int i = 0; i < kMaxBlocks; ++i) {
            if (!entries_[i].active) {
                entries_[i].active = true;
                entries_[i].ip = ip;
                entries_[i].expire_tick = (duration_ms == 0) ? kPermanent : now_ms_() + duration_ms;
                ++total_blocks_;

                // 注入动态防火墙规则
                FwRule rule{};
                rule.match_src_ip = true;
                rule.src_ip = ip;
                rule.action = FwAction::DROP;
                FirewallEngine::instance().get_rule_table().add_rule(rule);

                record_(ip, "blocked");
                return true;
            }
        }
        return false; // 封禁表满
    }

    // 解封源 IP
    bool unblock_ip(uint32_t ip) {
        for (int i = 0; i < kMaxBlocks; ++i) {
            if (entries_[i].active && entries_[i].ip == ip) {
                entries_[i].active = false;
                const int r = find_block_rule_(ip);
                if (r >= 0)
                    FirewallEngine::instance().get_rule_table().delete_rule(r);
                record_(ip, "unblocked");
                return true;
            }
        }
        return false;
    }

    bool is_blocked(uint32_t ip) const {
        for (int i = 0; i < kMaxBlocks; ++i)
            if (entries_[i].active && entries_[i].ip == ip)
                return true;
        return false;
    }

    // 到期解封
    void tick() {
        const uint32_t now = now_ms_();
        for (int i = 0; i < kMaxBlocks; ++i) {
            if (entries_[i].active && entries_[i].expire_tick != kPermanent && now >= entries_[i].expire_tick) {
                unblock_ip(entries_[i].ip);
            }
        }
    }

    int get_block_count() const {
        int n = 0;
        for (int i = 0; i < kMaxBlocks; ++i)
            if (entries_[i].active)
                ++n;
        return n;
    }

    uint32_t get_total_blocks() const noexcept {
        return total_blocks_;
    }

    const char* get_last_action() const noexcept {
        return last_action_;
    }

    void reset() {
        for (int i = 0; i < kMaxBlocks; ++i) {
            if (entries_[i].active) {
                const int r = find_block_rule_(entries_[i].ip);
                if (r >= 0)
                    FirewallEngine::instance().get_rule_table().delete_rule(r);
                entries_[i].active = false;
            }
        }
        total_blocks_ = 0;
        last_action_[0] = '\0';
    }

private:
    struct BlockEntry {
        bool active = false;
        uint32_t ip = 0;
        uint32_t expire_tick = 0;
    };

    BlockEntry entries_[kMaxBlocks]{};
    uint32_t total_blocks_ = 0;
    char last_action_[64]{};

    static uint32_t now_ms_() noexcept {
        return tick_count;
    }

    // 在防火墙规则表中定位本模块注入的封禁规则
    static int find_block_rule_(uint32_t ip) {
        const FwRule* rules = FirewallEngine::instance().get_rule_table().get_rules();
        for (int i = 0; i < RuleTable::MAX_RULES; ++i) {
            const FwRule& r = rules[i];
            if (r.enabled && r.match_src_ip && r.src_ip == ip && r.action == FwAction::DROP &&
                !r.match_dst_ip && !r.match_protocol && !r.match_src_port && !r.match_dst_port &&
                !r.match_interface) {
                return i;
            }
        }
        return -1;
    }

    void record_(uint32_t ip, const char* what) {
        char* p = last_action_;
        const char* const end = last_action_ + sizeof(last_action_) - 1;
        while (*what && p < end)
            *p++ = *what++;
        if (p < end)
            *p++ = ' ';
        // 十进制 IP（简化：仅打印原始数值）
        char digits[12];
        int n = 0;
        uint32_t v = ip;
        do {
            digits[n++] = static_cast<char>('0' + (v % 10u));
            v /= 10u;
        } while (v && n < 12);
        while (n > 0 && p < end)
            *p++ = digits[--n];
        *p = '\0';
    }
};

} // namespace response
} // namespace aurora

#endif // AURORA_RESPONSE_AUTO_BLOCK_HPP
