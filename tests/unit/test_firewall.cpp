#include <gtest/gtest.h>
#include "net/firewall/firewall_engine.hpp"

class FirewallTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto& rules = FirewallEngine::instance().get_rule_table();
        for (int i = 0; i < RuleTable::MAX_RULES; i++) {
            rules.delete_rule(i);
        }
    }
};

TEST_F(FirewallTest, DefaultAccept) {
    uint8_t dummy_packet[64] = {0};
    // IP header: version 4, IHL 5 (20 bytes) -> 0x45
    dummy_packet[0] = 0x45;
    dummy_packet[9] = 17; // UDP protocol
    EXPECT_TRUE(FirewallEngine::instance().process_packet(dummy_packet, sizeof(dummy_packet), "wlan0"));
}

TEST_F(FirewallTest, AddAndMatchRule) {
    auto& rules = FirewallEngine::instance().get_rule_table();
    
    FwRule rule;
    rule.enabled = true;
    rule.match_protocol = true;
    rule.protocol = 6; // TCP
    rule.match_dst_port = true;
    rule.dst_port = 80;
    rule.action = FwAction::DROP;

    EXPECT_TRUE(rules.add_rule(rule));

    uint8_t pkt[64] = {0};
    pkt[0] = 0x45; // IPv4
    pkt[9] = 6;    // TCP
    
    // IP header is 20 bytes. TCP dest port is at offset 2 of TCP header (22 in packet)
    pkt[22] = 0;
    pkt[23] = 80;
    
    // DROP action means process_packet returns false (packet dropped)
    EXPECT_FALSE(FirewallEngine::instance().process_packet(pkt, sizeof(pkt), "wlan0"));
}
