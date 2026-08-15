/**
 * February SoftBus real-adapter host test
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror -I. \
 *       -o /tmp/test_softbus tests/unit/test_february_softbus.cpp
 *   /tmp/test_softbus
 */
#include "ai/february/softbus.hpp"
#include "ai/february/softbus_codec.hpp"
#include "ai/february/softbus_oh_adapter.hpp"
#include "ai/february/service.hpp"

#include <cstdio>
#include <cstring>
#include <cassert>
#include <vector>

using namespace aurora::february;

// ---- mock transport state -------------------------------------------------
static int g_create_calls = 0;
static int g_send_calls = 0;
static int g_open_calls = 0;
static SoftBusSessionId g_last_sid = kInvalidSession;
static std::vector<uint8_t> g_last_tx;

static int mock_create(const char*, const char*, void*) {
    ++g_create_calls;
    return 0;
}
static SoftBusSessionId mock_open(const char*, const char*, const char* net,
                                  void*) {
    ++g_open_calls;
    assert(net && net[0]);
    g_last_sid = 100 + g_open_calls;
    return g_last_sid;
}
static int mock_send(SoftBusSessionId sid, const void* data, unsigned len,
                     void*) {
    ++g_send_calls;
    assert(sid == g_last_sid);
    g_last_tx.assign(static_cast<const uint8_t*>(data),
                     static_cast<const uint8_t*>(data) + len);
    return 0;
}

// OH-style mocks
static int oh_create(const char*, const char*, const void*) { return 0; }
static int oh_open(const char*, const char*, const char*, const char*,
                   const void*) {
    return 77;
}
static int oh_send(int sid, const void* data, unsigned len) {
    g_last_sid = sid;
    g_last_tx.assign(static_cast<const uint8_t*>(data),
                     static_cast<const uint8_t*>(data) + len);
    ++g_send_calls;
    return 0;
}

int main() {
    std::printf("=== February SoftBus adapter test ===\n");

    // --- codec round-trip ---
    Intent in;
    in.type = IntentType::QueryStatus;
    in.confidence_x1000 = 900;
    in.source_id = 3;
    in.param0 = -5;
    in.param1 = 42;
    std::snprintf(in.text, sizeof(in.text), "status please");

    uint8_t frame[kSoftBusFrameMax];
    unsigned n = softbus_pack_intent(in, 99, 12345, frame, sizeof(frame));
    assert(n > 0);

    Intent out;
    uint32_t peer = 0, ts = 0;
    assert(softbus_unpack_intent(frame, n, out, peer, ts));
    assert(out.type == IntentType::QueryStatus);
    assert(out.confidence_x1000 == 900);
    assert(out.source_id == 3);
    assert(out.param0 == -5);
    assert(out.param1 == 42);
    assert(std::strcmp(out.text, "status please") == 0);
    assert(peer == 99);
    assert(ts == 12345);

    // bad magic
    frame[0] ^= 0xff;
    assert(!softbus_unpack_intent(frame, n, out, peer, ts));
    frame[0] ^= 0xff;

    // --- SoftBus + mock transport ---
    SoftBus& bus = SoftBus::instance();
    bus.clear();

    SoftBusTransportOps ops;
    ops.create_server = mock_create;
    ops.open_session = mock_open;
    ops.send_bytes = mock_send;
    bus.bind_transport(ops);
    assert(bus.start_server() == 0);
    assert(g_create_calls == 1);
    assert(bus.server_up());

    assert(bus.register_peer(7, "net-device-7"));
    SoftBusSessionId sid = bus.ensure_session(7);
    assert(sid >= 0);
    assert(g_open_calls == 1);

    g_send_calls = 0;
    g_last_tx.clear();
    Intent remote;
    remote.type = IntentType::Help;
    remote.confidence_x1000 = 950;
    assert(bus.publish_intent(7, remote, 5000));
    assert(g_send_calls == 1);
    assert(!g_last_tx.empty());

    // Simulate peer RX of the same frame into another logical bus (same process)
    SoftBus::instance().on_bytes_received(sid, g_last_tx.data(),
                                          static_cast<unsigned>(g_last_tx.size()));
    assert(bus.pending() >= 1);  // publish also loopback-enqueued + rx

    SoftBusMessage msg;
    unsigned drained = 0;
    while (bus.pop(msg)) {
        ++drained;
        assert(msg.intent.type == IntentType::Help ||
               msg.intent.type == IntentType::QueryStatus);
    }
    assert(drained >= 1);
    assert(bus.rx_count() >= 1);

    // --- OH adapter path ---
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
    assert(osid == 77);

    Intent greet;
    greet.type = IntentType::Greeting;
    greet.confidence_x1000 = 800;
    assert(SoftBus::instance().publish_intent(3, greet, 9000));
    assert(g_send_calls == 1);

    // Platform listener forwards bytes
    SoftBus::instance().clear();
    OhSoftBusAdapter::forward_bytes(77, g_last_tx.data(),
                                    static_cast<unsigned>(g_last_tx.size()));
    assert(SoftBus::instance().pending() == 1);
    SoftBusMessage m2;
    assert(SoftBus::instance().pop(m2));
    assert(m2.intent.type == IntentType::Greeting);

    // --- Service integration ---
    FebruaryService& svc = FebruaryService::instance();
    // re-bind mock for service path
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
    svc.publish_remote(0, help, 10000);  // loopback
    svc.run_once(10000);
    assert(FebruaryCore::instance().memory().last_intent().type ==
           IntentType::Help);

    svc.stop();

    std::printf("tx=%u rx=%u fails=%u\n",
                (unsigned)SoftBus::instance().tx_count(),
                (unsigned)SoftBus::instance().rx_count(),
                (unsigned)SoftBus::instance().tx_fail());
    std::printf("=== ALL SOFTBUS CHECKS PASSED ===\n");
    return 0;
}
