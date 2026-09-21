#include <gtest/gtest.h>
#include "net/ble/hci/hci_uart_transport.hpp"
#include "net/ble/hci/hci_packet.hpp"
#include "net/ble/hal_ble_impl.hpp"
#include "net/ble/ble_stealth.hpp"
#include "net/ble/ble_ids.hpp"
#include "net/ble/ble_mitm.hpp"
#include "net/ble/gatt_auditor.hpp"
#include "net/ble/nimble_bridge.hpp"
#include "kernel/core/device.hpp"
#include <vector>
#include <string>

using namespace auroraos::ble;
using namespace auroraos::ble::hci;

// ---------------------------------------------------------------------------
// Mock CharDevice for UART loopback and recording
// ---------------------------------------------------------------------------
class LoopbackUartDevice : public CharDevice {
public:
    std::vector<uint8_t> tx_buffer;

    LoopbackUartDevice() : CharDevice("loopback_uart") {}

    int open() override { return 0; }
    int close() override { return 0; }

    int write(const char* buf, int len, int, void*) override {
        if (!buf || len <= 0) return 0;
        for (int i = 0; i < len; ++i) {
            tx_buffer.push_back(static_cast<uint8_t>(buf[i]));
        }
        return len;
    }

    void clear() {
        tx_buffer.clear();
    }
};

// ---------------------------------------------------------------------------
// Test Fixture
// ---------------------------------------------------------------------------
class BleStackIntegrationTest : public ::testing::Test {
protected:
    LoopbackUartDevice uart_dev;
    HciUartTransport transport{&uart_dev};

    void SetUp() override {
        transport.init();
        g_hci_transport = &transport;
        HalBle::init();
        BleIds::instance().clear_alerts();
        BleIds::instance().load_default_rules();
    }

    void TearDown() override {
        g_hci_transport = nullptr;
    }
};

// ---------------------------------------------------------------------------
// 1. 板级 UART 喂数机制测试 (feed_rx_byte & feed_rx_bytes)
// ---------------------------------------------------------------------------

TEST_F(BleStackIntegrationTest, FeedRxBytesBatchAndCounters) {
    // 构造一个完整的 LE Meta Event (Advertising Report)
    // H4 Type: 0x04
    // Event Code: 0x3E (LE Meta)
    // Param Len: 0x0D (13 bytes)
    // Sub-event: 0x02 (LE Advertising Report)
    // Num Reports: 0x01
    // Event Type: 0x00 (Connectable Undirected)
    // Addr Type: 0x00 (Public)
    // Addr: 11:22:33:44:55:66
    // Data Len: 0x01
    // Data: 0x00
    // RSSI: -60 (0xC4)
    const uint8_t raw_adv_packet[] = {
        0x04, 0x3E, 0x0D,
        0x02, 0x01, 0x00, 0x00,
        0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0x01, 0x00, 0xC4
    };

    uint32_t init_packets = transport.get_rx_packets();
    uint32_t init_bytes = transport.get_rx_bytes();

    // 批量喂数测试
    transport.feed_rx_bytes(raw_adv_packet, sizeof(raw_adv_packet));

    EXPECT_EQ(transport.get_rx_packets(), init_packets + 1);
    EXPECT_EQ(transport.get_rx_bytes(), init_bytes + sizeof(raw_adv_packet));
    EXPECT_EQ(transport.get_rx_errors(), 0u);
}

TEST_F(BleStackIntegrationTest, FeedRxGarbageToleranceAndErrorAccounting) {
    uint32_t init_errors = transport.get_rx_errors();

    // 喂入 5 个噪声/垃圾字节
    const uint8_t noise[] = {0xFF, 0xAA, 0x55, 0x00, 0x10};
    transport.feed_rx_bytes(noise, sizeof(noise));

    EXPECT_EQ(transport.get_rx_errors(), init_errors + sizeof(noise));

    // 随后喂入合法断开连接完成事件 (Disconnection Complete)
    // H4: 0x04, Event: 0x05, Len: 0x04, Status: 0x00, Handle: 0x0040, Reason: 0x13
    const uint8_t disconn_evt[] = {0x04, 0x05, 0x04, 0x00, 0x40, 0x00, 0x13};
    transport.feed_rx_bytes(disconn_evt, sizeof(disconn_evt));

    // 验证状态机从垃圾中自愈并成功解析数据包
    EXPECT_GE(transport.get_rx_packets(), 1u);
}

// ---------------------------------------------------------------------------
// 2. BleStealth 广播隐身伪装测试
// ---------------------------------------------------------------------------

TEST_F(BleStackIntegrationTest, BleStealthPresetAirTagGeneration) {
    BleStealth& stealth = BleStealth::instance();
    stealth.set_preset(BleStealthPreset::AIRTAG);
    EXPECT_EQ(stealth.get_preset(), BleStealthPreset::AIRTAG);

    uint8_t ad_buf[BLE_ADV_MAX_LEN] = {0};
    size_t ad_len = stealth.build_advertisement(ad_buf, sizeof(ad_buf));

    // 验证 AD 数据长度符合蓝牙规范 (<= 31 字节)
    ASSERT_LE(ad_len, 31u);
    ASSERT_GE(ad_len, 25u);

    // 验证 iBeacon 厂商标识 (Apple 0x004C, 小端: 0x4C, 0x00)
    // 结构: [Len] [0xFF] [0x4C] [0x00] [0x02] [0x15] [UUID: 16B] [Major: 2B] [Minor: 2B] [TX Power: 1B]
    bool found_apple_mfg = false;
    size_t offset = 0;
    while (offset < ad_len) {
        uint8_t len = ad_buf[offset];
        if (len == 0 || offset + 1 + len > ad_len) break;
        uint8_t type = ad_buf[offset + 1];
        if (type == 0xFF && len >= 5) {
            uint16_t company = ad_buf[offset + 2] | (ad_buf[offset + 3] << 8);
            if (company == APPLE_COMPANY_ID) {
                found_apple_mfg = true;
                EXPECT_EQ(ad_buf[offset + 4], IBEACON_TYPE); // 0x02
                EXPECT_EQ(ad_buf[offset + 5], IBEACON_DATA_LEN); // 0x15 (21 bytes)
            }
        }
        offset += 1 + len;
    }
    EXPECT_TRUE(found_apple_mfg);
}

TEST_F(BleStackIntegrationTest, BleStealthRawAdvertisingInjection) {
    uart_dev.clear();

    // 触发原始隐身广告广播注入
    uint8_t dummy_ad[30];
    memset(dummy_ad, 0xA5, sizeof(dummy_ad));
    HalBle::start_advertising_raw(dummy_ad, sizeof(dummy_ad));

    // 检查是否有发送 HCI 命令至 UART (应包含 Set Adv Params, Set Adv Data, Adv Enable)
    ASSERT_GT(uart_dev.tx_buffer.size(), 10u);

    // 第一个发出的应为 H4 Command (0x01)
    EXPECT_EQ(uart_dev.tx_buffer[0], 0x01);
    EXPECT_TRUE(hal_ble_is_advertising());

    // 停止广播
    HalBle::stop_advertising();
    EXPECT_FALSE(hal_ble_is_advertising());
}

// ---------------------------------------------------------------------------
// 3. NimBLE Host 桥接与协调器测试
// ---------------------------------------------------------------------------

TEST_F(BleStackIntegrationTest, NimbleBridgeLifecycle) {
    NimbleBridge& bridge = NimbleBridge::instance();

    // 初始化桥接
    EXPECT_TRUE(bridge.init());
    EXPECT_TRUE(bridge.is_synced());

    // 调度单步执行
    bridge.step(0);

    // 启动与停止广播
    EXPECT_TRUE(bridge.start_advertising());
    EXPECT_TRUE(bridge.is_advertising());

    EXPECT_TRUE(bridge.stop_advertising());
    EXPECT_FALSE(bridge.is_advertising());

    // 模拟主机同步与复位
    bridge.on_host_sync();
    EXPECT_TRUE(bridge.is_synced());
    EXPECT_TRUE(bridge.is_advertising());

    bridge.on_host_reset(0);
    EXPECT_FALSE(bridge.is_synced());
    EXPECT_FALSE(bridge.is_advertising());
}

// ---------------------------------------------------------------------------
// 4. 链路安全与入侵检测联动全通电测试 (MITM, GattAuditor, BleIds)
// ---------------------------------------------------------------------------

TEST_F(BleStackIntegrationTest, ConnectionEventPowersSecurityEngines) {
    // 模拟 Controller 发来 LE Connection Complete 事件 (0x3E, sub 0x01)
    // H4: 0x04
    // Event: 0x3E, Param Len: 0x13 (19)
    // Sub: 0x01
    // Status: 0x00 (Success)
    // Conn Handle: 0x0042
    // Role: 0x01 (Slave)
    // Peer Addr Type: 0x00 (Public)
    // Peer Addr: AA:BB:CC:DD:EE:FF
    // Conn Interval: 0x0020 (40ms)
    // Latency: 0x0000
    // Supervision Timeout: 0x0064 (1000ms)
    // Master Clock Accuracy: 0x00
    const uint8_t conn_complete_pkt[] = {
        0x04, 0x3E, 0x13,
        0x01, 0x00, 0x42, 0x00,
        0x01, 0x00,
        0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA,
        0x20, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00
    };

    transport.feed_rx_bytes(conn_complete_pkt, sizeof(conn_complete_pkt));

    // 验证连接句柄与对端 MAC 是否同步到了 HalBle 单例
    EXPECT_EQ(hal_ble_get_conn_handle(), 0x0042);
    uint8_t mac_out[6] = {0};
    EXPECT_TRUE(hal_ble_query_conn_mac(0x0042, mac_out));
    EXPECT_EQ(mac_out[0], 0xFF);
    EXPECT_EQ(mac_out[5], 0xAA);

    // 验证 GattAuditor 是否注册了该设备
    const uint8_t peer_mac[6] = {0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
    auto* dev = GattAuditor::instance().register_device(peer_mac);
    ASSERT_NE(dev, nullptr);
    EXPECT_TRUE(dev->connected);

    // 验证 BleMitmDetector 建立了活跃会话
    EXPECT_GE(BleMitmDetector::instance().active_session_count(), 1);

    // 模拟快速重复发起 11 次连接，触发 BleIds 的 ConnectFlood 泛洪攻击检测
    for (int i = 0; i < 11; ++i) {
        transport.feed_rx_bytes(conn_complete_pkt, sizeof(conn_complete_pkt));
    }
    EXPECT_GE(BleIds::instance().event_count(), 1);
}

TEST_F(BleStackIntegrationTest, AclSmpWeakPairingTriggersIdsAlert) {
    // 先建立连接 Handle 0x0042 -> MAC AA:BB:CC:DD:EE:FF
    const uint8_t peer_mac[6] = {0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
    hal_ble_remember_conn_mac(0x0042, peer_mac);

    // 构造一个 SMP Pairing Request (CID 0x0006, Code 0x01)
    // AuthReq 设为 0x00 (无 MITM, 无 LE Secure Connections, 弱配对 JustWorks)
    // Max Key Size = 7 (极低秘钥长度，脆弱加密)
    // H4 Type: 0x02 (ACL Data)
    // Handle 0x0042, PB=0, BC=0 -> 0x42, 0x00
    // Total Len: 11 bytes (0x0B, 0x00)
    // L2CAP Len: 7 bytes (0x07, 0x00)
    // L2CAP CID: 0x0006 (SMP)
    // SMP Code: 0x01 (Pairing Request)
    // IO Cap: 0x03 (NoInputNoOutput)
    // OOB: 0x00
    // AuthReq: 0x00 (JustWorks, No SC)
    // MaxKeySize: 0x07 (7 bytes)
    // InitKeyDist: 0x01
    // RespKeyDist: 0x01
    const uint8_t weak_smp_acl[] = {
        0x02, 0x42, 0x00, 0x0B, 0x00,
        0x07, 0x00, 0x06, 0x00,
        0x01, 0x03, 0x00, 0x00, 0x07, 0x01, 0x01
    };

    int init_events = BleIds::instance().event_count();
    transport.feed_rx_bytes(weak_smp_acl, sizeof(weak_smp_acl));

    // 验证 BleIds 捕获到 WeakSecurity 事件
    EXPECT_GT(BleIds::instance().event_count(), init_events);

    // 评估规则库并检查告警产生
    BleIds::instance().scan_and_evaluate();
}

TEST_F(BleStackIntegrationTest, AclAttGattDiscoveryAudit) {
    const uint8_t peer_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    hal_ble_remember_conn_mac(0x0050, peer_mac);
    GattAuditor::instance().register_device(peer_mac);

    // 构造通过 ATT 上报的已知脆弱服务特征值 (ANCS: 0x7905 或 HID: 0x1812)
    // ATT Read by Type Response (Opcode 0x09)
    // H4 Type: 0x02 (ACL)
    // Handle: 0x0050
    // Total Len: 13 bytes
    // L2CAP Len: 9 bytes
    // L2CAP CID: 0x0004 (ATT)
    // Opcode: 0x09 (Read by Type Response)
    // Item Len: 7
    // Item 1: Handle: 0x0014, Props: 0x08 (Write), ValHandle: 0x0015, UUID: 0x1802 (Immediate Alert)
    const uint8_t att_char_acl[] = {
        0x02, 0x50, 0x00, 0x0D, 0x00,
        0x09, 0x00, 0x04, 0x00,
        0x09, 0x07,
        0x14, 0x00, 0x08, 0x15, 0x00, 0x02, 0x18
    };

    transport.feed_rx_bytes(att_char_acl, sizeof(att_char_acl));

    // 验证 GattAuditor 注册并审计了该特征值
    auto* dev = GattAuditor::instance().register_device(peer_mac);
    ASSERT_NE(dev, nullptr);
    EXPECT_GE(dev->char_count, 1);
    EXPECT_EQ(dev->characteristics[0].uuid16, 0x1802);
}
