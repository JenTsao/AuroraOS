// test_ids.cpp — 嵌入式 NIDS 单元测试
//
// 覆盖：
//   - 协议异常：畸形 IHL
//   - TCP 标志位异常：NULL 扫描
//   - 端口扫描 / SYN 洪水
//   - ARP 欺骗
//   - DNS 隧道（超长名 / TXT 记录）
//   - 载荷特征（Aho-Corasick 多模式匹配）
//   - 良性流量不告警

#include <gtest/gtest.h>
#include "../../security/ids/ids_engine.hpp"

using namespace aurora::ids;

extern volatile uint32_t tick_count;

namespace {

const uint32_t IP_SRC = 0x0A000001; // 10.0.0.1
const uint32_t IP_DST = 0x0A000002; // 10.0.0.2

// 填充以太网 + IPv4 头（20 字节），返回 L4 起始偏移
int fill_eth_ipv4(uint8_t* p, uint8_t proto, uint32_t src, uint32_t dst) {
    int n = 0;
    for (int i = 0; i < 6; ++i)
        p[n++] = 0xAA; // dst MAC
    for (int i = 0; i < 6; ++i)
        p[n++] = 0xBB; // src MAC
    p[n++] = 0x08;
    p[n++] = 0x00; // IPv4
    p[n++] = 0x45; // v4, IHL = 5
    p[n++] = 0x00; // TOS
    p[n++] = 0x00;
    p[n++] = 0x00; // total_len（finish 时回填）
    p[n++] = 0x00;
    p[n++] = 0x00; // id
    p[n++] = 0x00;
    p[n++] = 0x00; // flags/frag
    p[n++] = 0x40; // TTL
    p[n++] = proto;
    p[n++] = 0x00;
    p[n++] = 0x00; // checksum
    p[n++] = static_cast<uint8_t>(src >> 24);
    p[n++] = static_cast<uint8_t>(src >> 16);
    p[n++] = static_cast<uint8_t>(src >> 8);
    p[n++] = static_cast<uint8_t>(src);
    p[n++] = static_cast<uint8_t>(dst >> 24);
    p[n++] = static_cast<uint8_t>(dst >> 16);
    p[n++] = static_cast<uint8_t>(dst >> 8);
    p[n++] = static_cast<uint8_t>(dst);
    return n;
}

int fill_tcp(uint8_t* p, int off, uint16_t sport, uint16_t dport, uint8_t flags,
             const uint8_t* payload, int plen) {
    p[off + 0] = static_cast<uint8_t>(sport >> 8);
    p[off + 1] = static_cast<uint8_t>(sport);
    p[off + 2] = static_cast<uint8_t>(dport >> 8);
    p[off + 3] = static_cast<uint8_t>(dport);
    for (int i = 4; i < 12; ++i)
        p[off + i] = 0; // seq + ack
    p[off + 12] = 0x50; // data offset = 5
    p[off + 13] = flags;
    for (int i = 14; i < 20; ++i)
        p[off + i] = 0; // window/checksum/urg
    int n = off + 20;
    for (int i = 0; i < plen; ++i)
        p[n++] = payload[i];
    return n;
}

int fill_udp(uint8_t* p, int off, uint16_t sport, uint16_t dport, const uint8_t* payload, int plen) {
    p[off + 0] = static_cast<uint8_t>(sport >> 8);
    p[off + 1] = static_cast<uint8_t>(sport);
    p[off + 2] = static_cast<uint8_t>(dport >> 8);
    p[off + 3] = static_cast<uint8_t>(dport);
    p[off + 4] = static_cast<uint8_t>((8 + plen) >> 8);
    p[off + 5] = static_cast<uint8_t>(8 + plen);
    p[off + 6] = 0;
    p[off + 7] = 0; // checksum
    int n = off + 8;
    for (int i = 0; i < plen; ++i)
        p[n++] = payload[i];
    return n;
}

// 回填 IPv4 total_len（= 帧长 - 以太网头 14）
void finish(uint8_t* p, int total_len) {
    const uint16_t v = static_cast<uint16_t>(total_len - 14);
    p[16] = static_cast<uint8_t>(v >> 8);
    p[17] = static_cast<uint8_t>(v);
}

int fill_arp(uint8_t* p, uint16_t op, uint32_t spa, const uint8_t sha[6]) {
    int n = 0;
    for (int i = 0; i < 6; ++i)
        p[n++] = 0xFF; // dst（广播）
    for (int i = 0; i < 6; ++i)
        p[n++] = sha[i]; // src
    p[n++] = 0x08;
    p[n++] = 0x06; // ARP
    p[n++] = 0x00;
    p[n++] = 0x01; // htype
    p[n++] = 0x08;
    p[n++] = 0x00; // ptype
    p[n++] = 0x06; // hlen
    p[n++] = 0x04; // plen
    p[n++] = static_cast<uint8_t>(op >> 8);
    p[n++] = static_cast<uint8_t>(op);
    for (int i = 0; i < 6; ++i)
        p[n++] = sha[i]; // sender MAC
    p[n++] = static_cast<uint8_t>(spa >> 24);
    p[n++] = static_cast<uint8_t>(spa >> 16);
    p[n++] = static_cast<uint8_t>(spa >> 8);
    p[n++] = static_cast<uint8_t>(spa);
    for (int i = 0; i < 6; ++i)
        p[n++] = 0x00; // target MAC
    p[n++] = static_cast<uint8_t>(IP_DST >> 24);
    p[n++] = static_cast<uint8_t>(IP_DST >> 16);
    p[n++] = static_cast<uint8_t>(IP_DST >> 8);
    p[n++] = static_cast<uint8_t>(IP_DST);
    return n;
}

} // namespace

class IdsEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        tick_count = 1000;
        IdsEngine::instance().reset();
        IdsEngine::instance().enable(true);
        IdsEngine::instance().init();
    }

    bool has_alert(IdsCategory c) const {
        const uint32_t total = IdsEngine::instance().get_alert_count();
        for (uint32_t i = 0; i < total; ++i) {
            const IdsAlert* a = IdsEngine::instance().get_alert(static_cast<int>(i));
            if (a && a->category == c)
                return true;
        }
        return false;
    }
};

TEST_F(IdsEngineTest, DetectsMalformedIhl) {
    uint8_t pkt[64] = {0};
    const int off = fill_eth_ipv4(pkt, 6, IP_SRC, IP_DST);
    pkt[14] = 0x44; // IHL = 4（< 5，畸形）

    IdsEngine::instance().process_packet(pkt, off);

    EXPECT_TRUE(has_alert(IdsCategory::MalformedPacket));
}

TEST_F(IdsEngineTest, DetectsNullScan) {
    uint8_t pkt[64] = {0};
    const int off = fill_eth_ipv4(pkt, 6, IP_SRC, IP_DST);
    const int total = fill_tcp(pkt, off, 12345, 80, 0x00 /* 无任何标志 */, nullptr, 0);
    finish(pkt, total);

    IdsEngine::instance().process_packet(pkt, total);

    EXPECT_TRUE(has_alert(IdsCategory::TcpFlagAnomaly));
}

TEST_F(IdsEngineTest, DetectsPortScan) {
    uint8_t pkt[64] = {0};
    for (int i = 0; i < 12; ++i) {
        const int off = fill_eth_ipv4(pkt, 6, IP_SRC, IP_DST);
        const int total = fill_tcp(pkt, off, static_cast<uint16_t>(20000 + i), static_cast<uint16_t>(80 + i),
                                   0x02 /* SYN */, nullptr, 0);
        finish(pkt, total);
        IdsEngine::instance().process_packet(pkt, total);
    }

    EXPECT_TRUE(has_alert(IdsCategory::PortScan));
}

TEST_F(IdsEngineTest, DetectsSynFlood) {
    uint8_t pkt[64] = {0};
    for (int i = 0; i < 51; ++i) {
        const int off = fill_eth_ipv4(pkt, 6, IP_SRC, IP_DST);
        const int total = fill_tcp(pkt, off, 20000, 80, 0x02 /* SYN，同一端口 */, nullptr, 0);
        finish(pkt, total);
        IdsEngine::instance().process_packet(pkt, total);
    }

    EXPECT_TRUE(has_alert(IdsCategory::SynFlood));
}

TEST_F(IdsEngineTest, DetectsArpSpoof) {
    uint8_t macA[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t macB[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x66};
    uint8_t pkt[64] = {0};

    int total = fill_arp(pkt, 2 /* reply */, IP_SRC, macA);
    IdsEngine::instance().process_packet(pkt, total);

    total = fill_arp(pkt, 2 /* reply */, IP_SRC, macB); // 同 IP 不同 MAC
    IdsEngine::instance().process_packet(pkt, total);

    EXPECT_TRUE(has_alert(IdsCategory::ArpSpoof));
}

TEST_F(IdsEngineTest, DetectsPayloadSignature) {
    const uint8_t payload[] = "/bin/sh";
    uint8_t pkt[64] = {0};
    const int off = fill_eth_ipv4(pkt, 6, IP_SRC, IP_DST);
    const int total = fill_tcp(pkt, off, 12345, 80, 0x10 /* ACK */, payload, sizeof(payload) - 1);
    finish(pkt, total);

    IdsEngine::instance().process_packet(pkt, total);

    EXPECT_TRUE(has_alert(IdsCategory::PayloadSignature));
}

TEST_F(IdsEngineTest, DetectsDnsTunnelLongName) {
    uint8_t pkt[256] = {0};
    const int off = fill_eth_ipv4(pkt, 17 /* UDP */, IP_SRC, IP_DST);

    uint8_t dns[128] = {0};
    dns[5] = 0x01; // qdcount = 1
    int dn = 12;
    // 4 个标签 × 20 字符 = 84 字节编码名 > 52 阈值
    for (int l = 0; l < 4; ++l) {
        dns[dn++] = 20;
        for (int i = 0; i < 20; ++i)
            dns[dn++] = 'a';
    }
    dns[dn++] = 0x00; // 名称结束
    dns[dn++] = 0x00;
    dns[dn++] = 0x01; // qtype A
    dns[dn++] = 0x00;
    dns[dn++] = 0x01; // qclass IN

    const int total = fill_udp(pkt, off, 53000, 53, dns, dn);
    finish(pkt, total);
    IdsEngine::instance().process_packet(pkt, total);

    EXPECT_TRUE(has_alert(IdsCategory::DnsTunnel));
}

TEST_F(IdsEngineTest, DetectsDnsTunnelTxtRecord) {
    uint8_t pkt[128] = {0};
    const int off = fill_eth_ipv4(pkt, 17 /* UDP */, IP_SRC, IP_DST);

    uint8_t dns[32] = {0};
    dns[5] = 0x01; // qdcount = 1
    int dn = 12;
    dns[dn++] = 3;
    dns[dn++] = 'w';
    dns[dn++] = 'w';
    dns[dn++] = 'w';
    dns[dn++] = 0x00; // 名称结束
    dns[dn++] = 0x00;
    dns[dn++] = 0x10; // qtype TXT = 16
    dns[dn++] = 0x00;
    dns[dn++] = 0x01;

    const int total = fill_udp(pkt, off, 53000, 53, dns, dn);
    finish(pkt, total);
    IdsEngine::instance().process_packet(pkt, total);

    EXPECT_TRUE(has_alert(IdsCategory::DnsTunnel));
}

TEST_F(IdsEngineTest, NoAlertOnBenignTcp) {
    uint8_t pkt[64] = {0};
    const int off = fill_eth_ipv4(pkt, 6, IP_SRC, IP_DST);
    const int total = fill_tcp(pkt, off, 12345, 80, 0x10 /* ACK */, nullptr, 0);
    finish(pkt, total);

    IdsEngine::instance().process_packet(pkt, total);

    EXPECT_EQ(IdsEngine::instance().get_alert_count(), 0u);
}
