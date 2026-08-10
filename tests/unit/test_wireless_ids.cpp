#include <gtest/gtest.h>
#include "../../net/wireless/wireless_ids.hpp"

// Use the external tick_count from kernel_stubs.cpp for the test environment
extern volatile uint32_t tick_count;

class WirelessIdsTest : public ::testing::Test {
protected:
    void SetUp() override {
        tick_count = 1000;
        WirelessIds::instance().init();
        WirelessIds::instance().clear_rules();
        // Since we cannot easily clear alerts and events (private arrays),
        // we test relative increments or use unique setups.
    }
};

TEST_F(WirelessIdsTest, AddAndMatchRule) {
    auto& ids = WirelessIds::instance();
    
    IdsRule rule;
    rule.event_type = WirelessEventType::DeauthFlood;
    rule.min_severity = 50;
    rule.threshold_count = 2; // Needs 2 events in window
    rule.window_ms = 5000;
    rule.action = 1; // Alert
    rule.enabled = true;

    EXPECT_TRUE(ids.add_rule(rule));
    
    uint32_t start_alerts = ids.get_total_alerts();
    
    uint8_t bssid[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    
    // First event (should not trigger alert because threshold is 2)
    printf("Before 1st event: tick=%u, total=%u\n", tick_count, ids.get_total_alerts());
    ids.submit_event_simple(WirelessEventType::DeauthFlood, bssid, -50, 0);
    printf("After 1st event: tick=%u, total=%u\n", tick_count, ids.get_total_alerts());
    
    // Second event (within window, should trigger alert)
    tick_count += 1000; // 1 second later
    printf("Before 2nd event: tick=%u, total=%u\n", tick_count, ids.get_total_alerts());
    ids.submit_event_simple(WirelessEventType::DeauthFlood, bssid, -50, 0);
    printf("After 2nd event: tick=%u, total=%u\n", tick_count, ids.get_total_alerts());
    
    EXPECT_GT(ids.get_total_alerts(), start_alerts);
}

TEST_F(WirelessIdsTest, DefaultRulesLoad) {
    auto& ids = WirelessIds::instance();
    ids.clear_rules();
    ids.load_default_rules();
    
    EXPECT_GT(ids.get_rule_count(), 0);
}
