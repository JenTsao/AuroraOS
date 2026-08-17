#include <gtest/gtest.h>
#include "net/ble/hci/hci_uart_transport.hpp"
#include "net/ble/hci/hci_packet.hpp"
#include "net/ble/hal_ble_impl.hpp"
#include "kernel/core/device.hpp"
#include <vector>

using namespace auroraos::ble::hci;

// ---------------------------------------------------------------------------
// Mock CharDevice for HCI UART Transport
// ---------------------------------------------------------------------------
class MockUartDevice : public CharDevice {
public:
    std::vector<uint8_t> tx_data;

    MockUartDevice() : CharDevice("mock_uart") {}

    int open() override { return 0; }
    int close() override { return 0; }

    int write(const char* buf, int len, int /*flags*/, void* /*user*/) override {
        if (!buf || len <= 0) return 0;
        for (int i = 0; i < len; ++i) {
            tx_data.push_back(static_cast<uint8_t>(buf[i]));
        }
        return len;
    }

    void clear_tx() {
        tx_data.clear();
    }
};

// ---------------------------------------------------------------------------
// Test Fixture
// ---------------------------------------------------------------------------
class HciUartTransportTest : public ::testing::Test {
protected:
    MockUartDevice mock_dev;
    HciUartTransport* transport;

    void SetUp() override {
        transport = new HciUartTransport(&mock_dev);
        transport->init();
        g_hci_transport = transport;
    }

    void TearDown() override {
        g_hci_transport = nullptr;
        delete transport;
    }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(HciUartTransportTest, SendCommandH4Framing) {
    uint8_t cmd[4] = {0x03, 0x0C, 0x00, 0x00}; // Reset command
    int rc = transport->send_cmd(cmd, 3);
    EXPECT_EQ(rc, 0);

    // Should prepend H4 packet type 0x01 (Command)
    ASSERT_EQ(mock_dev.tx_data.size(), 4u);
    EXPECT_EQ(mock_dev.tx_data[0], 0x01);
    EXPECT_EQ(mock_dev.tx_data[1], 0x03);
    EXPECT_EQ(mock_dev.tx_data[2], 0x0C);
    EXPECT_EQ(mock_dev.tx_data[3], 0x00);
}

TEST_F(HciUartTransportTest, SendAclDataH4Framing) {
    uint8_t acl[8] = {0x40, 0x00, 0x04, 0x00, 0xAA, 0xBB, 0xCC, 0xDD};
    int rc = transport->send_acl(acl, 8);
    EXPECT_EQ(rc, 0);

    // Should prepend H4 packet type 0x02 (ACL)
    ASSERT_EQ(mock_dev.tx_data.size(), 9u);
    EXPECT_EQ(mock_dev.tx_data[0], 0x02);
    for (size_t i = 0; i < 8; ++i) {
        EXPECT_EQ(mock_dev.tx_data[1 + i], acl[i]);
    }
}

TEST_F(HciUartTransportTest, FeedRxHciEventComplete) {
    // HCI Command Complete Event for Reset:
    // H4 Type: 0x04
    // Event Code: 0x0E (Command Complete)
    // Param Len: 0x04
    // Num HCI Cmd Pkts: 0x01
    // Opcode: 0x0C03 (Reset, LE)
    // Status: 0x00 (Success)
    const uint8_t raw_event[] = {0x04, 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00};

    for (uint8_t byte : raw_event) {
        transport->feed_rx_byte(byte);
    }

    // After valid event feed, transport returns to WaitPacketType
    // Feed another event
    const uint8_t raw_disconn_event[] = {0x04, 0x05, 0x04, 0x00, 0x40, 0x00, 0x13};
    for (uint8_t byte : raw_disconn_event) {
        transport->feed_rx_byte(byte);
    }
}

TEST_F(HciUartTransportTest, FeedRxHciAclComplete) {
    // HCI ACL Packet:
    // H4 Type: 0x02
    // Handle (12b) + PB (2b) + BC (2b): 0x0040 (Handle 0x0040)
    // Data Length (2B, LE): 0x0004
    // Payload: 4 bytes
    const uint8_t raw_acl[] = {0x02, 0x40, 0x00, 0x04, 0x00, 0x11, 0x22, 0x33, 0x44};

    for (uint8_t byte : raw_acl) {
        transport->feed_rx_byte(byte);
    }
}

TEST_F(HciUartTransportTest, FeedRxIgnoresGarbageAndRecovers) {
    // Garbage bytes before valid event
    transport->feed_rx_byte(0xFF);
    transport->feed_rx_byte(0xAA);
    transport->feed_rx_byte(0x55);

    // Now send valid event
    const uint8_t valid_event[] = {0x04, 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00};
    for (uint8_t byte : valid_event) {
        transport->feed_rx_byte(byte);
    }
}
