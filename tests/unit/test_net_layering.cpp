#include <gtest/gtest.h>
#include <string.h>
#include <stdio.h>

#include "../../net/net_device.hpp"
#include "../../net/link_layer.hpp"
#include "../../net/protocol_registry.hpp"
#include "../../net/packet_capture.hpp"
#include "../../net/distributed_bus.hpp"
#include "../../net/device_route_table.hpp"
#include "../../net/ble/hci/hci_uart_transport.hpp"
#include "../../net/ble/hci/hci_event_dispatch.hpp"
#include "../../utils/hmac_sha256.hpp"

namespace {

// 模拟底层网卡设备（不依赖 StellarisEth 硬件寄存器）
class MockNetDevice final : public NetDevice {
public:
    MockNetDevice() {
        const uint8_t default_mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
        memcpy(mac_address_, default_mac, 6);
        set_name("en0");
    }

    bool init() override {
        initialized_ = true;
        return true;
    }

    bool send_frame(const uint8_t* data, int len) override {
        if (!data || len <= 0 || len > static_cast<int>(sizeof(last_tx_))) {
            return false;
        }
        memcpy(last_tx_, data, static_cast<size_t>(len));
        last_tx_len_ = len;
        tx_count_++;
        return true;
    }

    int receive_frame(uint8_t* buffer, int max_len) override {
        if (!has_rx_ || !buffer || max_len < rx_len_) {
            return 0;
        }
        memcpy(buffer, rx_buf_, static_cast<size_t>(rx_len_));
        int out = rx_len_;
        has_rx_ = false;
        rx_len_ = 0;
        return out;
    }

    void enqueue_rx(const uint8_t* data, int len) {
        if (!data || len <= 0 || len > static_cast<int>(sizeof(rx_buf_))) {
            return;
        }
        memcpy(rx_buf_, data, static_cast<size_t>(len));
        rx_len_ = len;
        has_rx_ = true;
    }

    bool initialized_ = false;
    uint8_t last_tx_[1514]{};
    int last_tx_len_ = 0;
    int tx_count_ = 0;

private:
    uint8_t rx_buf_[1514]{};
    int rx_len_ = 0;
    bool has_rx_ = false;
};

// 构造标准 Ethernet + IPv4 + UDP 数据帧
size_t build_eth_ipv4_udp_frame(uint8_t* out, size_t max_len, const uint8_t src_ip[4], const uint8_t dst_ip[4],
                                uint16_t src_port, uint16_t dst_port, const uint8_t* payload, size_t payload_len) {
    const size_t udp_len = 8 + payload_len;
    const size_t ip_len = 20 + udp_len;
    const size_t total_len = 14 + ip_len;
    if (!out || total_len > max_len) {
        return 0;
    }

    memset(out, 0, total_len);
    // Ethernet Header (14B)
    const uint8_t dst_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint8_t src_mac[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    memcpy(out + 0, dst_mac, 6);
    memcpy(out + 6, src_mac, 6);
    out[12] = 0x08;
    out[13] = 0x00; // EtherType: IPv4

    // IPv4 Header (20B)
    uint8_t* ip = out + 14;
    ip[0] = 0x45; // Version 4, IHL 5 (20B)
    ip[1] = 0x00;
    ip[2] = static_cast<uint8_t>((ip_len >> 8) & 0xFF);
    ip[3] = static_cast<uint8_t>(ip_len & 0xFF);
    ip[8] = 64; // TTL
    ip[9] = auroraos::net::PROTO_UDP;
    memcpy(ip + 12, src_ip, 4);
    memcpy(ip + 16, dst_ip, 4);

    // UDP Header (8B)
    uint8_t* udp = ip + 20;
    udp[0] = static_cast<uint8_t>((src_port >> 8) & 0xFF);
    udp[1] = static_cast<uint8_t>(src_port & 0xFF);
    udp[2] = static_cast<uint8_t>((dst_port >> 8) & 0xFF);
    udp[3] = static_cast<uint8_t>(dst_port & 0xFF);
    udp[4] = static_cast<uint8_t>((udp_len >> 8) & 0xFF);
    udp[5] = static_cast<uint8_t>(udp_len & 0xFF);

    if (payload && payload_len > 0) {
        memcpy(udp + 8, payload, payload_len);
    }
    return total_len;
}

// 构造标准 Ethernet + IPv4 + TCP 数据帧
size_t build_eth_ipv4_tcp_frame(uint8_t* out, size_t max_len, const uint8_t src_ip[4], const uint8_t dst_ip[4],
                                uint16_t src_port, uint16_t dst_port, uint8_t tcp_flags, const uint8_t* payload,
                                size_t payload_len) {
    const size_t tcp_hdr_len = 20;
    const size_t ip_len = 20 + tcp_hdr_len + payload_len;
    const size_t total_len = 14 + ip_len;
    if (!out || total_len > max_len) {
        return 0;
    }

    memset(out, 0, total_len);
    out[12] = 0x08;
    out[13] = 0x00; // IPv4

    uint8_t* ip = out + 14;
    ip[0] = 0x45;
    ip[2] = static_cast<uint8_t>((ip_len >> 8) & 0xFF);
    ip[3] = static_cast<uint8_t>(ip_len & 0xFF);
    ip[8] = 64;
    ip[9] = auroraos::net::PROTO_TCP;
    memcpy(ip + 12, src_ip, 4);
    memcpy(ip + 16, dst_ip, 4);

    uint8_t* tcp = ip + 20;
    tcp[0] = static_cast<uint8_t>((src_port >> 8) & 0xFF);
    tcp[1] = static_cast<uint8_t>(src_port & 0xFF);
    tcp[2] = static_cast<uint8_t>((dst_port >> 8) & 0xFF);
    tcp[3] = static_cast<uint8_t>(dst_port & 0xFF);
    tcp[12] = 0x50; // Data offset: 5 words (20B)
    tcp[13] = tcp_flags;

    if (payload && payload_len > 0) {
        memcpy(tcp + tcp_hdr_len, payload, payload_len);
    }
    return total_len;
}

class MockUartCharDevice final : public CharDevice {
public:
    MockUartCharDevice() : CharDevice("mock_ble_uart") {}

    int read(char*, int, int, void*) override {
        return 0;
    }

    int write(const char*, int len, int, void*) override {
        return len;
    }
};

class NetLayeringTest : public ::testing::Test {
protected:
    void SetUp() override {
        auroraos::net::LinkLayer::instance().reset();
        auroraos::net::ProtocolRegistry::instance().reset();
    }

    void TearDown() override {
        auroraos::net::LinkLayer::instance().reset();
        auroraos::net::ProtocolRegistry::instance().reset();
        auroraos::ble::hci::ensure_ble_protocols_registered();
    }
};

} // namespace

// ============================================================
// 1. NetDevice 混杂模式解耦验证：PacketCapture -> LinkLayer -> NetDevice
// ============================================================
TEST_F(NetLayeringTest, NetDeviceAndLinkLayerPromiscuousDecoupling) {
    MockNetDevice mock_eth;
    auto& link = auroraos::net::LinkLayer::instance();
    ASSERT_TRUE(link.register_device("en0", &mock_eth, auroraos::net::LinkType::Ethernet));
    EXPECT_EQ(link.get_default_device(), &mock_eth);
    EXPECT_FALSE(mock_eth.is_promiscuous_mode());

    PacketCapture::instance().init();
    int fd = VfsManager::instance().open("/dev/pcap0", 0);
    ASSERT_GE(fd, 0);

    // 通过 /dev/pcap0 ioctl 开启混杂模式，验证通过 LinkLayer 转发至抽象 NetDevice
    EXPECT_EQ(VfsManager::instance().ioctl(fd, PacketCapture::IOCTL_ENABLE_PROMISC, nullptr), 0);
    EXPECT_TRUE(link.is_promiscuous_mode("en0"));
    EXPECT_TRUE(mock_eth.is_promiscuous_mode());

    // 通过 ioctl 关闭混杂模式
    EXPECT_EQ(VfsManager::instance().ioctl(fd, PacketCapture::IOCTL_DISABLE_PROMISC, nullptr), 0);
    EXPECT_FALSE(link.is_promiscuous_mode("en0"));
    EXPECT_FALSE(mock_eth.is_promiscuous_mode());

    // 再次开启混杂模式后关闭文件描述符，验证 close_file 自动恢复非混杂模式
    EXPECT_EQ(VfsManager::instance().ioctl(fd, PacketCapture::IOCTL_ENABLE_PROMISC, nullptr), 0);
    EXPECT_TRUE(mock_eth.is_promiscuous_mode());
    EXPECT_EQ(VfsManager::instance().close(fd), 0);
    EXPECT_FALSE(mock_eth.is_promiscuous_mode());
}

// ============================================================
// 2. LinkLayer RX/TX 管线测试：Tap 旁路嗅探 + Filter 过滤阻断
// ============================================================
TEST_F(NetLayeringTest, LinkLayerRxTxPipelineWithPacketCaptureAndFilter) {
    MockNetDevice mock_eth;
    auto& link = auroraos::net::LinkLayer::instance();
    ASSERT_TRUE(link.register_device("en0", &mock_eth, auroraos::net::LinkType::Ethernet));

    PacketCapture::instance().init();
    int fd = VfsManager::instance().open("/dev/pcap0", 0);
    ASSERT_GE(fd, 0);

    const uint8_t src_ip[4] = {10, 0, 2, 15};
    const uint8_t dst_ip[4] = {10, 0, 2, 2};
    const uint8_t payload[] = {'H', 'E', 'L', 'L', 'O'};
    uint8_t frame[128]{};
    size_t frame_len =
        build_eth_ipv4_udp_frame(frame, sizeof(frame), src_ip, dst_ip, 12345, 8080, payload, sizeof(payload));
    ASSERT_GT(frame_len, 0u);

    // 1) 入站帧经由 LinkLayer::receive_frame -> 自动触发 PacketCapture ILinkTap
    mock_eth.enqueue_rx(frame, static_cast<int>(frame_len));
    uint8_t rx_out[128]{};
    int rx_bytes = link.receive_frame("en0", rx_out, sizeof(rx_out));
    EXPECT_EQ(rx_bytes, static_cast<int>(frame_len));

    // 2) 出站帧经由 LinkLayer::transmit_frame -> 自动触发 PacketCapture ILinkTap 并送达网卡
    EXPECT_TRUE(link.transmit_frame("en0", frame, static_cast<int>(frame_len)));
    EXPECT_EQ(mock_eth.tx_count_, 1);

    CaptureStats stats = PacketCapture::instance().get_stats();
    EXPECT_EQ(stats.packets_captured, 2u);

    // 3) 注册阻断过滤器：拒绝长度等于 frame_len 的帧
    static int s_blocked_len = 0;
    s_blocked_len = static_cast<int>(frame_len);
    ASSERT_TRUE(link.register_filter_fn("test_blocker",
                                        [](const uint8_t*, int len, auroraos::net::PacketDirection,
                                           auroraos::net::LinkType, const char*, void*) -> bool {
                                            return len != s_blocked_len;
                                        }));

    mock_eth.enqueue_rx(frame, static_cast<int>(frame_len));
    EXPECT_EQ(link.receive_frame("en0", rx_out, sizeof(rx_out)), 0);
    EXPECT_FALSE(link.transmit_frame("en0", frame, static_cast<int>(frame_len)));
    EXPECT_EQ(mock_eth.tx_count_, 1); // 未新增硬件发送

    VfsManager::instance().close(fd);
}

// ============================================================
// 3. ProtocolRegistry 优先级、精确端口匹配、裁决语义与畸形帧防护
// ============================================================
TEST_F(NetLayeringTest, ProtocolRegistryPrioritySpecificityAndVerdicts) {
    struct TraceHandler final : public auroraos::net::IProtocolHandler {
        const char* handler_name;
        auroraos::net::ProtocolVerdict verdict;
        int* order_counter;
        int invoked_at = 0;
        int call_count = 0;
        uint16_t last_dst_port = 0;

        TraceHandler(const char* n, auroraos::net::ProtocolVerdict v, int* counter)
            : handler_name(n), verdict(v), order_counter(counter) {}

        const char* name() const noexcept override {
            return handler_name;
        }

        auroraos::net::ProtocolVerdict on_packet(const auroraos::net::L4PacketContext& ctx, const uint8_t*,
                                                 size_t) noexcept override {
            call_count++;
            invoked_at = ++(*order_counter);
            last_dst_port = ctx.dst_port;
            return verdict;
        }
    };

    int order = 0;
    TraceHandler tap_handler("tap_all", auroraos::net::ProtocolVerdict::Pass, &order);
    TraceHandler mdns_service("mdns_5353", auroraos::net::ProtocolVerdict::Consume, &order);
    TraceHandler udp_fallback("lwip_udp_fallback", auroraos::net::ProtocolVerdict::Pass, &order);
    TraceHandler telnet_blocker("telnet_drop", auroraos::net::ProtocolVerdict::Drop, &order);

    auto& reg = auroraos::net::ProtocolRegistry::instance();
    ASSERT_TRUE(reg.register_handler(auroraos::net::PROTO_UDP, auroraos::net::ProtocolRegistry::PORT_ANY, &udp_fallback,
                                     auroraos::net::ProtocolRegistry::PRIORITY_FALLBACK));
    ASSERT_TRUE(reg.register_handler(auroraos::net::PROTO_UDP, 5353, &mdns_service,
                                     auroraos::net::ProtocolRegistry::PRIORITY_SERVICE));
    ASSERT_TRUE(reg.register_handler(auroraos::net::PROTO_ANY, auroraos::net::ProtocolRegistry::PORT_ANY, &tap_handler,
                                     auroraos::net::ProtocolRegistry::PRIORITY_TAP));
    ASSERT_TRUE(reg.register_handler(auroraos::net::PROTO_TCP, 23, &telnet_blocker,
                                     auroraos::net::ProtocolRegistry::PRIORITY_SECURITY));

    const uint8_t src_ip[4] = {192, 168, 1, 10};
    const uint8_t dst_ip[4] = {224, 0, 0, 251};
    const uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t frame[128]{};

    // A) 发送至 UDP 5353：tap_all 先执行 (Pass)，随后 mdns_5353 执行并返回 Consume，udp_fallback 不再触发
    size_t len = build_eth_ipv4_udp_frame(frame, sizeof(frame), src_ip, dst_ip, 5353, 5353, data, sizeof(data));
    EXPECT_EQ(reg.dispatch_ethernet_frame(frame, static_cast<int>(len), auroraos::net::PacketDirection::Rx, "en0"),
              auroraos::net::ProtocolVerdict::Consume);
    EXPECT_EQ(tap_handler.call_count, 1);
    EXPECT_EQ(mdns_service.call_count, 1);
    EXPECT_EQ(udp_fallback.call_count, 0);
    EXPECT_LT(tap_handler.invoked_at, mdns_service.invoked_at);

    // B) 发送至 UDP 9000：tap_all 与 udp_fallback 均被调用
    len = build_eth_ipv4_udp_frame(frame, sizeof(frame), src_ip, dst_ip, 1000, 9000, data, sizeof(data));
    EXPECT_EQ(reg.dispatch_ethernet_frame(frame, static_cast<int>(len), auroraos::net::PacketDirection::Rx, "en0"),
              auroraos::net::ProtocolVerdict::Pass);
    EXPECT_EQ(tap_handler.call_count, 2);
    EXPECT_EQ(mdns_service.call_count, 1);
    EXPECT_EQ(udp_fallback.call_count, 1);
    EXPECT_EQ(udp_fallback.last_dst_port, 9000u);

    // C) 发送至 TCP 23：telnet_blocker 返回 Drop，LinkLayer::input_frame 阻断该帧
    len = build_eth_ipv4_tcp_frame(frame, sizeof(frame), src_ip, dst_ip, 44444, 23, 0x02, data, sizeof(data));
    EXPECT_EQ(auroraos::net::LinkLayer::instance().input_frame(frame, static_cast<int>(len),
                                                               auroraos::net::LinkType::Ethernet, 0, "en0"),
              auroraos::net::ProtocolVerdict::Drop);
    EXPECT_EQ(telnet_blocker.call_count, 1);

    // D) 畸形/截断帧边界安全：不足 14B、不足 IPv4 头、截断 UDP 头均安全返回 Pass
    const uint8_t short_frame[10] = {0};
    EXPECT_EQ(reg.dispatch_ethernet_frame(short_frame, static_cast<int>(sizeof(short_frame)),
                                          auroraos::net::PacketDirection::Rx, "en0"),
              auroraos::net::ProtocolVerdict::Pass);
    EXPECT_EQ(reg.dispatch_ethernet_frame(frame, 20, auroraos::net::PacketDirection::Rx, "en0"),
              auroraos::net::ProtocolVerdict::Pass);
}

// ============================================================
// 4. DistributedSoftBus 可插拔 L4 协议注册验证 (UDP 8899)
// ============================================================
TEST_F(NetLayeringTest, DistributedSoftBusPluggableUdpRegistration) {
    DeviceRouteTable::instance().reset();
    auto& bus = DistributedSoftBus::instance();
    bus.reset_security_state();
    bus.init();

    const uint8_t psk[32] = {
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x11, 0x21, 0x31, 0x41, 0x51, 0x61, 0x71, 0x81,
        0x12, 0x22, 0x32, 0x42, 0x52, 0x62, 0x72, 0x82, 0x13, 0x23, 0x33, 0x43, 0x53, 0x63, 0x73, 0x83,
    };
    bus.provision_key(psk, 1);

    // 构造合法的 HMAC-SHA256 软总线 Beacon 载荷
    const char* peer_id = "watch_peer_01";
    const uint8_t seq_bytes[4] = {0, 0, 0, 1};
    HmacSha256 hmac;
    hmac.init(psk, 32);
    hmac.update(reinterpret_cast<const uint8_t*>(peer_id), strlen(peer_id));
    hmac.update(seq_bytes, 4);
    uint8_t digest[32]{};
    hmac.finish(digest);

    char auth_hex[65]{};
    const char* hex = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        auth_hex[i * 2] = hex[(digest[i] >> 4) & 0x0F];
        auth_hex[i * 2 + 1] = hex[digest[i] & 0x0F];
    }

    char beacon_json[256]{};
    snprintf(beacon_json, sizeof(beacon_json),
             "{\"event\":\"beacon\",\"device_id\":\"%s\","
             "\"cap\":[\"display\",\"touch\"],\"seq\":\"1\",\"auth\":\"%s\"}",
             peer_id, auth_hex);

    const uint8_t src_ip[4] = {192, 168, 10, 42};
    const uint8_t dst_ip[4] = {255, 255, 255, 255};
    uint8_t frame[512]{};
    size_t frame_len = build_eth_ipv4_udp_frame(frame, sizeof(frame), src_ip, dst_ip, DistributedSoftBus::SOFTBUS_PORT,
                                                DistributedSoftBus::SOFTBUS_PORT,
                                                reinterpret_cast<const uint8_t*>(beacon_json), strlen(beacon_json));
    ASSERT_GT(frame_len, 0u);

    // 直接通过 LinkLayer::input_frame 注入以太网帧，验证自动解复用至 ProtocolRegistry -> DistributedSoftBus。
    // L4 处理后返回 Consume：SoftBus 是 8899 的唯一处理路径（不再放行回
    // socket 二次处理，见 distributed_bus.hpp on_packet）。
    EXPECT_EQ(auroraos::net::LinkLayer::instance().input_frame(frame, static_cast<int>(frame_len),
                                                               auroraos::net::LinkType::Ethernet, 0, "en0"),
              auroraos::net::ProtocolVerdict::Consume);

    char discovered_ip[16]{};
    ASSERT_TRUE(DeviceRouteTable::instance().find_device_ip(peer_id, discovered_ip));
    EXPECT_STREQ(discovered_ip, "192.168.10.42");

    // 验证防重放：相同 seq=1 再次注入应当被拒绝
    EXPECT_FALSE(bus.process_beacon_message("192.168.10.42", beacon_json, 100));
}

// ============================================================
// 5. BLE HCI 与 L2CAP (ATT / SMP / 自定义 CID) 可插拔链路与协议分发
// ============================================================
TEST_F(NetLayeringTest, BleHciAndL2capPluggableProtocolDispatch) {
    MockUartCharDevice mock_uart;
    auroraos::ble::hci::HciUartTransport transport(&mock_uart);
    transport.init();
    BleIds::instance().clear_alerts();
    BleIds::instance().load_default_rules();

    // 1) 在 LinkLayer 上注册 BLE 链路嗅探器，观察所有 BleHci 帧
    static int s_ble_tap_frames = 0;
    s_ble_tap_frames = 0;
    ASSERT_TRUE(auroraos::net::LinkLayer::instance().register_tap_fn(
        "ble_tap",
        [](const uint8_t*, int, auroraos::net::PacketDirection, auroraos::net::LinkType link_type, const char*, void*) {
            if (link_type == auroraos::net::LinkType::BleHci) {
                s_ble_tap_frames++;
            }
        }));

    // 2) 在 ProtocolRegistry 上注册自定义 BLE L2CAP CoC 信道处理器 (CID 0x0040)
    struct CustomL2capHandler final : public auroraos::net::IProtocolHandler {
        int rx_count = 0;
        uint16_t conn_handle = 0;
        char payload_str[16]{};

        const char* name() const noexcept override {
            return "ble_custom_coc";
        }

        auroraos::net::ProtocolVerdict on_packet(const auroraos::net::L4PacketContext& ctx, const uint8_t* payload,
                                                 size_t len) noexcept override {
            rx_count++;
            conn_handle = ctx.src_port;
            size_t copy_len = (len < sizeof(payload_str) - 1) ? len : (sizeof(payload_str) - 1);
            if (payload && copy_len > 0) {
                memcpy(payload_str, payload, copy_len);
                payload_str[copy_len] = '\0';
            }
            return auroraos::net::ProtocolVerdict::Consume;
        }
    } custom_coc;

    ASSERT_TRUE(auroraos::net::ProtocolRegistry::instance().register_handler(
        auroraos::net::PROTO_BLE_L2CAP, 0x0040, &custom_coc, auroraos::net::ProtocolRegistry::PRIORITY_SERVICE));

    // A) 注入 H4 LE Connection Complete Event (Handle = 0x0007)
    const uint8_t conn_evt[] = {
        0x04,                               // H4 Packet Type: HCI Event
        0x3E, 0x13,                         // EVT_LE_META, Param Len = 19
        0x01,                               // Subevent: LE_SUB_CONNECTION_COMPLETE
        0x00,                               // Status: Success
        0x07, 0x00,                         // Connection Handle: 0x0007
        0x01,                               // Role: Slave
        0x00,                               // Peer Addr Type: Public
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, // Peer MAC
        0x18, 0x00,                         // Interval
        0x00, 0x00,                         // Latency
        0xC8, 0x00,                         // Supervision Timeout
        0x00                                // Master Clock Accuracy
    };
    transport.feed_rx_bytes(conn_evt, sizeof(conn_evt));
    EXPECT_EQ(auroraos::ble::hal_ble_get_conn_handle(), 0x0007u);
    EXPECT_EQ(s_ble_tap_frames, 1);

    // B) 注入 H4 ACL SMP Weak Pairing Request (CID = 0x0006, MaxKeySize = 7 -> 触发 WeakSecurity IDS 事件)
    int events_before = BleIds::instance().event_count();
    const uint8_t smp_acl[] = {
        0x02,       // H4 Packet Type: HCI ACL
        0x07, 0x20, // Handle 0x0007, PB=0b10
        0x0B, 0x00, // ACL Length = 11
        0x07, 0x00, // L2CAP Length = 7
        0x06, 0x00, // L2CAP CID = 0x0006 (SMP)
        0x01,       // SMP Pairing Request
        0x03,       // IO Cap: NoInputNoOutput
        0x00,       // OOB: Not present
        0x01,       // AuthReq: Bonding, no MITM, no SC
        0x07,       // MaxKeySize = 7 (< 16, weak!)
        0x01, 0x01  // InitKeyDist, RespKeyDist
    };
    transport.feed_rx_bytes(smp_acl, sizeof(smp_acl));
    EXPECT_GT(BleIds::instance().event_count(), events_before);
    EXPECT_EQ(s_ble_tap_frames, 2);

    // C) 注入 H4 ACL 自定义 L2CAP CID 0x0040 数据包 -> 验证 ProtocolRegistry 分发到 custom_coc
    const uint8_t coc_acl[] = {0x02,       // H4 Packet Type: HCI ACL
                               0x07, 0x20, // Handle 0x0007
                               0x08, 0x00, // ACL Length = 8
                               0x04, 0x00, // L2CAP Length = 4
                               0x40, 0x00, // L2CAP CID = 0x0040
                               'P',  'I',  'N', 'G'};
    transport.feed_rx_bytes(coc_acl, sizeof(coc_acl));
    EXPECT_EQ(custom_coc.rx_count, 1);
    EXPECT_EQ(custom_coc.conn_handle, 0x0007u);
    EXPECT_STREQ(custom_coc.payload_str, "PING");
    EXPECT_EQ(s_ble_tap_frames, 3);
}
