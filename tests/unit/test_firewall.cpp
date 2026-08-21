#include <gtest/gtest.h>
#include "../../net/firewall/firewall_engine.hpp"

class FirewallTest : public ::testing::Test {
protected:
    void SetUp() override {
        FirewallEngine::instance().enable(true);
        auto& rules = FirewallEngine::instance().get_rule_table();
        for (int i = 0; i < RuleTable::MAX_RULES; i++) {
            rules.delete_rule(i);
        }
    }
};

TEST_F(FirewallTest, DefaultAccept) {
    uint8_t dummy_packet[64] = {0};
    // Ethernet header
    dummy_packet[12] = 0x08; // IPv4
    dummy_packet[13] = 0x00;
    // IP header: version 4, IHL 5 (20 bytes) -> 0x45
    dummy_packet[14] = 0x45;
    dummy_packet[14 + 9] = 17; // UDP protocol
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
    // Ethernet header: MAC dst (6), MAC src (6), EthType (2)
    pkt[12] = 0x08; // IPv4
    pkt[13] = 0x00;

    // IP header starts at 14
    pkt[14] = 0x45;  // IPv4, IHL=5 (20 bytes)
    pkt[14 + 9] = 6; // TCP

    // TCP header starts at 14 + 20 = 34
    // dest port is at offset 2 of TCP header = 34 + 2 = 36
    pkt[36] = 0;
    pkt[37] = 80;

    // DROP action means process_packet returns false (packet dropped)
    EXPECT_FALSE(FirewallEngine::instance().process_packet(pkt, sizeof(pkt), "wlan0"));
}

TEST_F(FirewallTest, MalformedIhlZeroDoesNotBypassRuleTable) {
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
    pkt[12] = 0x08; // IPv4
    pkt[13] = 0x00;

    // IP header starts at 14 with IHL = 0 (malformed)
    pkt[14] = 0x40; // Version 4, IHL = 0
    pkt[14 + 9] = 6; // TCP

    // Put 80 at offset 16 (where dst_port would misalign to if IHL=0 was used)
    pkt[16] = 0;
    pkt[17] = 80;

    // Rule table must reject malformed IHL and not match misaligned port offsets
    EXPECT_EQ(rules.match(pkt, sizeof(pkt), "wlan0"), FwAction::ACCEPT);
}

TEST_F(FirewallTest, MalformedIhlLessThanFiveDoesNotCrashOrMisalign) {
    auto& rules = FirewallEngine::instance().get_rule_table();

    FwRule rule;
    rule.enabled = true;
    rule.match_protocol = true;
    rule.protocol = 6;
    rule.match_dst_port = true;
    rule.dst_port = 80;
    rule.action = FwAction::DROP;

    EXPECT_TRUE(rules.add_rule(rule));

    for (uint8_t ihl = 0; ihl < 5; ++ihl) {
        uint8_t pkt[64] = {0};
        pkt[12] = 0x08;
        pkt[13] = 0x00;
        pkt[14] = 0x40 | ihl;
        pkt[14 + 9] = 6;

        EXPECT_EQ(rules.match(pkt, sizeof(pkt), "wlan0"), FwAction::ACCEPT);
    }
}
