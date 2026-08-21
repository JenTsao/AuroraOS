// =============================================================================
// test_stealth_identity.cpp — Unit tests for LAN Stealth & Camouflage Engine
// =============================================================================

#include <gtest/gtest.h>
#include "../../net/stealth_identity.hpp"
#include "../../net/net_device.hpp"

// Mock NetDevice implementation for unit testing MAC operations
class MockNetDevice : public NetDevice {
private:
    bool initialized_;

public:
    MockNetDevice() : initialized_(false) {
        memset(mac_address_, 0, sizeof(mac_address_));
    }

    bool init() override {
        initialized_ = true;
        return true;
    }

    bool is_initialized() const {
        return initialized_;
    }

    int receive_frame(uint8_t* /*buffer*/, int /*max_len*/) override {
        return 0;
    }

    bool send_frame(const uint8_t* /*buffer*/, int /*len*/) override {
        return true;
    }
};

// -----------------------------------------------------------------------------
// Test Preset Configurations & Protocol Stack Fingerprints
// -----------------------------------------------------------------------------

TEST(StealthIdentityTest, PresetConfigurationsAndFingerprints) {
    auto& si = StealthIdentity::instance();

    // 1. Apple iPad Preset
    si.set_active_preset(StealthIdentity::Preset::APPLE_IPAD);
    EXPECT_EQ(si.active_preset(), StealthIdentity::Preset::APPLE_IPAD);
    EXPECT_STREQ(si.get_hostname(), "iPad-of-Staff");
    EXPECT_EQ(si.get_target_ttl(), 64);
    EXPECT_EQ(si.get_target_tcp_window(), 65535);
    EXPECT_EQ(si.get_target_mss(), 1460);
    EXPECT_FALSE(si.is_icmp_echo_suppressed());
    EXPECT_FALSE(si.is_ghost_mode());

    uint8_t fp_len = 0;
    const uint8_t* fp = si.get_dhcp_fingerprint_data(fp_len);
    EXPECT_EQ(fp_len, 10u);
    EXPECT_NE(fp, nullptr);
    EXPECT_EQ(fp[0], 1); // Subnet Mask
    EXPECT_EQ(fp[1], 3); // Router
    EXPECT_EQ(fp[2], 6); // DNS Server

    // 2. HP LaserJet Preset
    si.set_active_preset(StealthIdentity::Preset::HP_LASERJET);
    EXPECT_STREQ(si.get_hostname(), "HP-LaserJet-M402dn");
    EXPECT_EQ(si.get_target_ttl(), 255);
    EXPECT_EQ(si.get_target_tcp_window(), 8192);
    fp = si.get_dhcp_fingerprint_data(fp_len);
    EXPECT_EQ(fp_len, 6u);

    // 3. Hikvision Camera Preset
    si.set_active_preset(StealthIdentity::Preset::HIKVISION_CAM);
    EXPECT_STREQ(si.get_hostname(), "HIKVISION-DS-2CD2042WD");
    EXPECT_EQ(si.get_target_ttl(), 64);
    EXPECT_EQ(si.get_target_tcp_window(), 29200);

    // 4. Cisco Switch Preset
    si.set_active_preset(StealthIdentity::Preset::CISCO_SWITCH);
    EXPECT_STREQ(si.get_hostname(), "cisco-catalyst-2960");
    EXPECT_EQ(si.get_target_ttl(), 255);
    EXPECT_EQ(si.get_target_tcp_window(), 4128);

    // 5. Silent Ghost Mode
    si.set_active_preset(StealthIdentity::Preset::SILENT_GHOST);
    EXPECT_TRUE(si.is_ghost_mode());
    EXPECT_EQ(si.get_hostname(), nullptr);
    EXPECT_TRUE(si.is_icmp_echo_suppressed());
    EXPECT_TRUE(si.is_gratuitous_arp_suppressed());
}

// -----------------------------------------------------------------------------
// Test MAC OUI Spoofing & High-Entropy Unicast Generation
// -----------------------------------------------------------------------------

TEST(StealthIdentityTest, MacOuiSpoofingAndUnicastSanity) {
    MockNetDevice dev;
    uint8_t mac[6];

    // Apply Apple iPad OUI (10:DD:B1)
    StealthIdentity::apply(dev, StealthIdentity::Preset::APPLE_IPAD, mac);
    EXPECT_EQ(mac[0], 0x10);
    EXPECT_EQ(mac[1], 0xDD);
    EXPECT_EQ(mac[2], 0xB1);

    // Validate Unicast & Global Unique bit flags
    EXPECT_EQ(mac[3] & 0x01, 0); // Bit 0 of Byte 0 in little endian word / Byte 3 LSB must be 0 (unicast)
    EXPECT_EQ(mac[3] & 0x02, 0); // Bit 1 of Byte 3 must be 0

    // Apply Hikvision OUI (00:18:AE)
    StealthIdentity::apply(dev, StealthIdentity::Preset::HIKVISION_CAM, mac);
    EXPECT_EQ(mac[0], 0x00);
    EXPECT_EQ(mac[1], 0x18);
    EXPECT_EQ(mac[2], 0xAE);

    // Ephemeral MAC Rotation test
    uint8_t mac1[6];
    StealthIdentity::instance().set_active_preset(StealthIdentity::Preset::APPLE_MACBOOK);
    StealthIdentity::rotate_ephemeral_mac(dev, mac1);
    EXPECT_EQ(mac1[0], 0x3C);
    EXPECT_EQ(mac1[1], 0x22);
    EXPECT_EQ(mac1[2], 0xFB);
}

// -----------------------------------------------------------------------------
// Test Inbound Probe Suppression & Decoy Banners
// -----------------------------------------------------------------------------

TEST(StealthIdentityTest, InboundProbeSuppressionAndDecoyBanners) {
    auto& si = StealthIdentity::instance();

    // Normal Apple iPad mode: does not drop ping or mDNS
    si.set_active_preset(StealthIdentity::Preset::APPLE_IPAD);
    EXPECT_FALSE(si.should_drop_inbound_probe(1 /* ICMP */, 0, 8 /* Echo Request */));
    EXPECT_FALSE(si.should_drop_inbound_probe(17 /* UDP */, 5353 /* mDNS */));

    // Ghost mode: drops Ping, mDNS, SSDP, LLMNR, NetBIOS
    si.set_active_preset(StealthIdentity::Preset::SILENT_GHOST);
    EXPECT_TRUE(si.should_drop_inbound_probe(1 /* ICMP */, 0, 8 /* Echo Request */));
    EXPECT_FALSE(si.should_drop_inbound_probe(1 /* ICMP */, 0, 0 /* Echo Reply */)); // Does not drop replies
    EXPECT_TRUE(si.should_drop_inbound_probe(17 /* UDP */, 5353 /* mDNS */));
    EXPECT_TRUE(si.should_drop_inbound_probe(17 /* UDP */, 1900 /* SSDP */));
    EXPECT_TRUE(si.should_drop_inbound_probe(17 /* UDP */, 5355 /* LLMNR */));
    EXPECT_TRUE(si.should_drop_inbound_probe(17 /* UDP */, 137 /* NetBIOS */));
    EXPECT_FALSE(si.should_drop_inbound_probe(17 /* UDP */, 53 /* DNS */));

    // Decoy banners
    si.set_active_preset(StealthIdentity::Preset::HP_LASERJET);
    EXPECT_STREQ(si.get_decoy_service_banner(9100), "HP JetDirect Ready\r\n");
    EXPECT_NE(si.get_decoy_service_banner(80), nullptr);

    si.set_active_preset(StealthIdentity::Preset::HIKVISION_CAM);
    EXPECT_STREQ(si.get_decoy_service_banner(554), "RTSP/1.0 200 OK\r\nServer: HIKVISION-RTSP/1.0\r\n");
    EXPECT_NE(si.get_decoy_service_banner(80), nullptr);

    si.set_active_preset(StealthIdentity::Preset::CISCO_SWITCH);
    EXPECT_STREQ(si.get_decoy_service_banner(23), "\r\nUser Access Verification\r\n\r\nPassword: ");
}
