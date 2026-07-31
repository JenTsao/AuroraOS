#include <gtest/gtest.h>
#include "../../net/wifi_driver.hpp"

using namespace auroraos::net;

TEST(WifiDriverTest, BasicConnectionFlow) {
    EspWifiDriver wifi;
    
    // Test initialization
    EXPECT_TRUE(wifi.init());
    
    // Initially link should be down
    EXPECT_FALSE(wifi.is_link_up());
    
    // Connect to mock AP
    bool connected = wifi.connect("test_ssid", "test_pass");
    EXPECT_TRUE(connected);
    EXPECT_TRUE(wifi.is_link_up());
    
    // Disconnect
    EXPECT_TRUE(wifi.disconnect());
    EXPECT_FALSE(wifi.is_link_up());
}

TEST(WifiDriverTest, FrameHandling) {
    EspWifiDriver wifi;
    
    // Should not send frame if disconnected
    uint8_t dummy_frame[64] = {0};
    EXPECT_FALSE(wifi.send_frame(dummy_frame, sizeof(dummy_frame)));
    
    wifi.connect("test_ssid", "test_pass");
    
    // Should send frame when connected
    EXPECT_TRUE(wifi.send_frame(dummy_frame, sizeof(dummy_frame)));
    
    // Receive frame should safely return 0 in mock mode
    int bytes_read = wifi.receive_frame(dummy_frame, sizeof(dummy_frame));
    EXPECT_EQ(bytes_read, 0);
}
