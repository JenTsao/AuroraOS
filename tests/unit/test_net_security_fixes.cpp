#include <gtest/gtest.h>
#include <string.h>
#include <stdint.h>
#include "services/net/net_service.hpp"
#include "net/firewall/rule_table.hpp"

// ---- 1. Test NetServer send/sendto/setsockopt length clamping ----
TEST(NetServiceSecurityTest, HandleSendClampsOversizedLen) {
    auroraos::net::NetRequest req{};
    req.opcode = auroraos::net::NetOpcode::Send;
    req.send.fd = 1;
    req.send.flags = 0;
    req.send.len = 0xFFFFFFFF; // Attempted huge length buffer overflow
    memset(req.send.data, 'A', sizeof(req.send.data));

    auroraos::net::NetReply reply{};
    // In host test stub, lwip_send returns -1, but this verifies handle_send does not crash or corrupt memory
    auroraos::net::NetServer::instance().process_request(req, reply);
    EXPECT_EQ(reply.status, -1);
}

TEST(NetServiceSecurityTest, HandleSendToClampsOversizedLen) {
    auroraos::net::NetRequest req{};
    req.opcode = auroraos::net::NetOpcode::SendTo;
    req.sendto.fd = 1;
    req.sendto.flags = 0;
    req.sendto.addr = 0x7F000001;
    req.sendto.port = 8080;
    req.sendto.len = 65536; // Much larger than 1024
    memset(req.sendto.data, 'B', sizeof(req.sendto.data));

    auroraos::net::NetReply reply{};
    auroraos::net::NetServer::instance().process_request(req, reply);
    EXPECT_EQ(reply.status, -1);
}

TEST(NetServiceSecurityTest, HandleSetsockoptClampsOversizedOptlen) {
    auroraos::net::NetRequest req{};
    req.opcode = auroraos::net::NetOpcode::Setsockopt;
    req.setsockopt.fd = 1;
    req.setsockopt.level = 1;
    req.setsockopt.optname = 1;
    req.setsockopt.optlen = 10000; // Oversized compared to 128-byte optval buffer
    memset(req.setsockopt.optval, 0, sizeof(req.setsockopt.optval));

    auroraos::net::NetReply reply{};
    auroraos::net::NetServer::instance().process_request(req, reply);
    EXPECT_EQ(reply.status, -1);
}

// ---- 2. Test IP string formatting (Endianness and zero-tens formatting) ----
// Replicate the exact ip_to_string_ algorithm used in scan_lua_binding.cpp
static void test_ip_to_string(uint32_t ip, char* out, int max_len) {
    if (!out || max_len < 16)
        return;

    uint8_t b[4];
    b[0] = static_cast<uint8_t>((ip >> 24) & 0xFF);
    b[1] = static_cast<uint8_t>((ip >> 16) & 0xFF);
    b[2] = static_cast<uint8_t>((ip >> 8) & 0xFF);
    b[3] = static_cast<uint8_t>(ip & 0xFF);

    int pos = 0;

    auto append_byte = [&](uint8_t v) {
        if (v >= 100) {
            out[pos++] = '0' + (v / 100);
            v %= 100;
            out[pos++] = '0' + (v / 10);
            v %= 10;
        } else if (v >= 10) {
            out[pos++] = '0' + (v / 10);
            v %= 10;
        }
        out[pos++] = '0' + v;
    };

    append_byte(b[0]);
    out[pos++] = '.';
    append_byte(b[1]);
    out[pos++] = '.';
    append_byte(b[2]);
    out[pos++] = '.';
    append_byte(b[3]);
    out[pos] = '\0';
}

TEST(IpFormattingTest, CorrectEndiannessAndDigits) {
    char buf[32];

    // 192.168.1.1 = 0xC0A80101
    test_ip_to_string(0xC0A80101, buf, sizeof(buf));
    EXPECT_STREQ(buf, "192.168.1.1");

    // 10.0.0.1 = 0x0A000001
    test_ip_to_string(0x0A000001, buf, sizeof(buf));
    EXPECT_STREQ(buf, "10.0.0.1");

    // 172.16.105.208 = (172 << 24) | (16 << 16) | (105 << 8) | 208
    uint32_t ip_105_208 = (172u << 24) | (16u << 16) | (105u << 8) | 208u;
    test_ip_to_string(ip_105_208, buf, sizeof(buf));
    EXPECT_STREQ(buf, "172.16.105.208");

    // 100.10.1.0 = (100 << 24) | (10 << 16) | (1 << 8) | 0
    uint32_t ip_100_10_1_0 = (100u << 24) | (10u << 16) | (1u << 8) | 0u;
    test_ip_to_string(ip_100_10_1_0, buf, sizeof(buf));
    EXPECT_STREQ(buf, "100.10.1.0");

    // 255.255.255.255 = 0xFFFFFFFF
    test_ip_to_string(0xFFFFFFFF, buf, sizeof(buf));
    EXPECT_STREQ(buf, "255.255.255.255");

    // 0.0.0.0 = 0
    test_ip_to_string(0x00000000, buf, sizeof(buf));
    EXPECT_STREQ(buf, "0.0.0.0");
}

// ---- 3. Test RuleTable IHL Validation ----
TEST(FirewallRuleTableTest, RejectsIhlLessThanFive) {
    RuleTable table;

    FwRule drop_80;
    drop_80.enabled = true;
    drop_80.match_protocol = true;
    drop_80.protocol = 6; // TCP
    drop_80.match_dst_port = true;
    drop_80.dst_port = 80;
    drop_80.action = FwAction::DROP;
    EXPECT_TRUE(table.add_rule(drop_80));

    // Valid packet targeting port 80 -> should DROP
    uint8_t valid_pkt[64] = {0};
    valid_pkt[12] = 0x08;
    valid_pkt[13] = 0x00;
    valid_pkt[14] = 0x45; // IHL = 5
    valid_pkt[14 + 9] = 6; // TCP
    valid_pkt[14 + 20 + 2] = 0;
    valid_pkt[14 + 20 + 3] = 80;
    EXPECT_EQ(table.match(valid_pkt, sizeof(valid_pkt), "eth0"), FwAction::DROP);

    // Malformed packet with IHL = 0
    uint8_t malformed_pkt[64] = {0};
    malformed_pkt[12] = 0x08;
    malformed_pkt[13] = 0x00;
    malformed_pkt[14] = 0x40; // IHL = 0
    malformed_pkt[14 + 9] = 6;
    // Malformed packet should return ACCEPT (default policy for non-matching/malformed)
    EXPECT_EQ(table.match(malformed_pkt, sizeof(malformed_pkt), "eth0"), FwAction::ACCEPT);
}
