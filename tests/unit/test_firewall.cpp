#include <gtest/gtest.h>
#include "net/firewall/firewall.hpp"

using namespace auroraos::net::firewall;

class FirewallTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clear rule table before each test
        auto& rules = FirewallEngine::instance().get_rule_table();
        for (int i = 0; i < RuleTable::MAX_RULES; i++) {
            rules.delete_rule(i);
        }
    }
};

TEST_F(FirewallTest, AddAndMatchRule) {
    auto& rules = FirewallEngine::instance().get_rule_table();
    
    FwRule rule;
    rule.match_protocol = true;
    rule.protocol = 6; // TCP
    rule.match_dst_port = true;
    rule.dst_port = 80;
    rule.action = FwAction::DROP;

    int idx = rules.add_rule(rule);
    EXPECT_GE(idx, 0);

    FwPacket pkt;
    pkt.protocol = 6;
    pkt.dst_port = 80;
    
    EXPECT_EQ(FirewallEngine::instance().process_packet(pkt), FwAction::DROP);
}

TEST_F(FirewallTest, DefaultAccept) {
    FwPacket pkt;
    pkt.protocol = 17; // UDP
    EXPECT_EQ(FirewallEngine::instance().process_packet(pkt), FwAction::ACCEPT);
}
