/**
 * February SoftBus real-adapter host test (GoogleTest).
 */
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "ai/february/softbus.hpp"
#include "ai/february/softbus_codec.hpp"
#include "ai/february/softbus_oh_adapter.hpp"
#include "ai/february/service.hpp"

using namespace aurora::february;

namespace {

int g_create_calls = 0;
int g_send_calls = 0;
int g_open_calls = 0;
SoftBusSessionId g_last_sid = kInvalidSession;
std::vector<uint8_t> g_last_tx;

int mock_create(const char*, const char*, void*) {
    ++g_create_calls;
    return 0;
}

SoftBusSessionId mock_open(const char*, const char*, const char* net, void*) {
    ++g_open_calls;
    EXPECT_TRUE(net && net[0]);
    g_last_sid = 100 + g_open_calls;
    return g_last_sid;
}

int mock_send(SoftBusSessionId sid, const void* data, unsigned len, void*) {
    ++g_send_calls;
    EXPECT_EQ(sid, g_last_sid);
    g_last_tx.assign(static_cast<const uint8_t*>(data),
                     static_cast<const uint8_t*>(data) + len);
    return 0;
}

int oh_create(const char*, const char*, const void*) { return 0; }
int oh_open(const char*, const char*, const char*, const char*, const void*) {
    return 77;
}
int oh_send(int sid, const void* data, unsigned len) {
    g_last_sid = sid;
    g_last_tx.assign(static_cast<const uint8_t*>(data),
                     static_cast<const uint8_t*>(data) + len);
    ++g_send_calls;
    return 0;
}

}  // namespace

class FebruarySoftBusTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_create_calls = 0;
        g_send_calls = 0;
        g_open_calls = 0;
        g_last_sid = kInvalidSession;
        g_last_tx.clear();
    }
};

TEST_F(FebruarySoftBusTest, CodecRoundTrip) {
    Intent in;
    in.type = IntentType::QueryStatus;
    in.confidence_x1000 = 900;
    in.source_id = 3;
    in.param0 = -5;
    in.param1 = 42;
    std::snprintf(in.text, sizeof(in.text), "status please");

    uint8_t frame[kSoftBusFrameMax];
    const unsigned n = softbus_pack_intent(in, 99, 12345, frame, sizeof(frame));
    ASSERT_GT(n, 0u);

    Intent out;
    uint32_t peer = 0, ts = 0;
    ASSERT_TRUE(softbus_unpack_intent(frame, n, out, peer, ts));
    EXPECT_EQ(out.type, IntentType::QueryStatus);
    EXPECT_EQ(out.confidence_x1000, 900u);
    EXPECT_EQ(out.source_id, 3u);
    EXPECT_EQ(out.param0, -5);
    EXPECT_EQ(out.param1, 42);
    EXPECT_STREQ(out.text, "status please");
    EXPECT_EQ(peer, 99u);
    EXPECT_EQ(ts, 12345u);

    frame[0] ^= 0xff;
    EXPECT_FALSE(softbus_unpack_intent(frame, n, out, peer, ts));
    frame[0] ^= 0xff;
}

TEST_F(FebruarySoftBusTest, MockTransportPublishAndReceive) {
    SoftBus& bus = SoftBus::instance();
    bus.clear();

    SoftBusTransportOps ops;
    ops.create_server = mock_create;
    ops.open_session = mock_open;
    ops.send_bytes = mock_send;
    bus.bind_transport(ops);
    EXPECT_EQ(bus.start_server(), 0);
    EXPECT_EQ(g_create_calls, 1);
    EXPECT_TRUE(bus.server_up());

    EXPECT_TRUE(bus.register_peer(7, "net-device-7"));
    SoftBusSessionId sid = bus.ensure_session(7);
    EXPECT_GE(sid, 0);
    EXPECT_EQ(g_open_calls, 1);

    g_send_calls = 0;
    g_last_tx.clear();
    Intent remote;
    remote.type = IntentType::Help;
    remote.confidence_x1000 = 950;
    EXPECT_TRUE(bus.publish_intent(7, remote, 5000));
    EXPECT_EQ(g_send_calls, 1);
    EXPECT_FALSE(g_last_tx.empty());

    SoftBus::instance().on_bytes_received(sid, g_last_tx.data(),
                                          static_cast<unsigned>(g_last_tx.size()));
    EXPECT_GE(bus.pending(), 1u);

    SoftBusMessage msg;
    unsigned drained = 0;
    while (bus.pop(msg)) {
        ++drained;
        EXPECT_TRUE(msg.intent.type == IntentType::Help ||
                    msg.intent.type == IntentType::QueryStatus);
    }
    EXPECT_GE(drained, 1u);
    EXPECT_GE(bus.rx_count(), 1u);
}

TEST_F(FebruarySoftBusTest, OhAdapterPath) {
    g_send_calls = 0;
    g_last_tx.clear();
    OhSoftBusFns oh;
    oh.create_session_server = oh_create;
    oh.open_session = oh_open;
    oh.send_bytes = oh_send;
    OhSoftBusAdapter::instance().bind(oh);

    SoftBus::instance().clear();
    SoftBus::instance().bind_transport(OhSoftBusAdapter::instance().ops());
    SoftBus::instance().start_server();
    SoftBus::instance().register_peer(3, "oh-peer-3");
    SoftBusSessionId osid = SoftBus::instance().ensure_session(3);
    EXPECT_EQ(osid, 77);

    Intent greet;
    greet.type = IntentType::Greeting;
    greet.confidence_x1000 = 800;
    EXPECT_TRUE(SoftBus::instance().publish_intent(3, greet, 9000));
    EXPECT_EQ(g_send_calls, 1);

    SoftBus::instance().clear();
    OhSoftBusAdapter::forward_bytes(77, g_last_tx.data(),
                                    static_cast<unsigned>(g_last_tx.size()));
    EXPECT_EQ(SoftBus::instance().pending(), 1u);
    SoftBusMessage m2;
    EXPECT_TRUE(SoftBus::instance().pop(m2));
    EXPECT_EQ(m2.intent.type, IntentType::Greeting);
}

TEST_F(FebruarySoftBusTest, ServiceIntegrationLoopback) {
    FebruaryService& svc = FebruaryService::instance();
    SoftBus::instance().clear();
    SoftBusTransportOps ops2;
    ops2.create_server = mock_create;
    ops2.open_session = mock_open;
    ops2.send_bytes = mock_send;
    svc.bind_softbus_transport(ops2);
    svc.start();

    Intent help;
    help.type = IntentType::Help;
    help.confidence_x1000 = 900;
    EXPECT_TRUE(svc.publish_remote(0, help, 10000));
    svc.run_once(10000);
    EXPECT_EQ(FebruaryCore::instance().memory().last_intent().type,
              IntentType::Help);

    svc.stop();
}
