/**
 * February reliability / stress host test (GoogleTest, Phase 2.2)
 *
 * Covers: EventBus overflow, SoftBus inbox drop-oldest, PeerTable reclaim,
 * Crit reentrancy balance, planner table swap, remote yield, time wrap,
 * codec edge frames, multi-run service lifecycle.
 *
 * Originally a standalone program with its own main() and hand-written
 * REL_CHECK assertions. Each former sub-routine is now an independent
 * GoogleTest case so failures are reported individually. The February
 * singletons are process-global, so the fixture installs/reset the critical
 * hooks and counters before every case; each case re-establishes the service
 * or bus state it needs.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "ai/february/service.hpp"
#include "ai/february/planner.hpp"
#include "ai/february/peer_table.hpp"
#include "ai/february/crit.hpp"
#include "ai/february/board_bind.hpp"
#include "ai/february/softbus.hpp"
#include "ai/february/softbus_codec.hpp"
#include "ai/february/event_bus.hpp"
#include "ai/february/cooldown.hpp"
#include "ai/february/string_util.hpp"

using namespace aurora::february;

namespace {

int g_crit_depth = 0;
int g_crit_enter = 0;
int g_crit_exit = 0;
int g_speak = 0;
int g_notify = 0;
bool g_crit_underflow = false;

void crit_enter(void*) {
    ++g_crit_enter;
    ++g_crit_depth;
}

void crit_exit(void*) {
    ++g_crit_exit;
    --g_crit_depth;
    if (g_crit_depth < 0) {
        g_crit_underflow = true;
    }
}

void h_speak(const char*, void*) { ++g_speak; }
void h_notify(const char*, void*) { ++g_notify; }

// Mock transport used by the multi-peer publish test so the real outbound TX
// path is exercised (without a transport, publish_intent correctly reports
// "no TX path" instead of silently pretending to deliver).
int rel_create(const char*, const char*, void*) { return 0; }
SoftBusSessionId rel_open(const char*, const char*, const char*, void*) {
    return 900;
}
int rel_send(SoftBusSessionId, const void*, unsigned, void*) { return 0; }

}  // namespace

class FebruaryReliabilityTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_crit_depth = 0;
        g_crit_enter = 0;
        g_crit_exit = 0;
        g_speak = 0;
        g_notify = 0;
        g_crit_underflow = false;
        FebruaryCrit::set(crit_enter, crit_exit);
        // A prior case may have cleared the process-global EventBus, which
        // also drops FebruaryCore's subscription while ready_ stays true.
        // reset() makes the next svc.start() re-run init() and re-subscribe.
        FebruaryCore::instance().reset();
        EventBus::instance().clear();
    }

    void TearDown() override {
        FebruaryCrit::set(nullptr, nullptr);
    }
};

TEST_F(FebruaryReliabilityTest, StringUtilHelpers) {
    EXPECT_TRUE(contains_ci("Hey February", "february"));
    EXPECT_FALSE(contains_ci("abc", "xyz"));
    char buf[4];
    EXPECT_EQ(copy_cstr(buf, sizeof(buf), "abcdef"), 3u);
    EXPECT_STREQ(buf, "abc");
    EXPECT_EQ(copy_cstr(buf, sizeof(buf), nullptr), 0u);
}

TEST_F(FebruaryReliabilityTest, EventBusOverflowDropsOldest) {
    EventBus::instance().clear();
    // Classic ring: one slot reserved -> usable capacity = depth - 1
    const unsigned depth = kEventQueueDepth;
    const unsigned usable = depth - 1;
    for (unsigned i = 0; i < depth + 5; ++i) {
        Event ev;
        ev.type = EventType::SystemTick;
        ev.timestamp_ms = i;
        ev.source_id = i;
        EventBus::instance().publish(ev);
    }
    EXPECT_GE(EventBus::instance().drop_count(), 5u);
    const unsigned n = EventBus::instance().process(depth + 10);
    EXPECT_EQ(n, usable);  // queue holds at most usable (= depth - 1)
    EXPECT_EQ(EventBus::instance().process(1), 0u);  // empty after drain
}

TEST_F(FebruaryReliabilityTest, EventBusHasLocalIntent) {
    EventBus::instance().clear();
    EXPECT_FALSE(EventBus::instance().has_local_intent());
    Event ev;
    ev.type = EventType::SensorUpdate;
    EventBus::instance().publish(ev);
    EXPECT_FALSE(EventBus::instance().has_local_intent());
    ev.type = EventType::IntentDetected;
    EventBus::instance().publish(ev);
    EXPECT_TRUE(EventBus::instance().has_local_intent());
    EventBus::instance().process(16);
    EXPECT_FALSE(EventBus::instance().has_local_intent());
}

TEST_F(FebruaryReliabilityTest, SoftBusInboxOverflow) {
    SoftBusStub::instance().clear();
    Intent in;
    in.type = IntentType::Help;
    in.confidence_x1000 = 900;
    const unsigned usable = FEBRUARY_SOFTBUS_QUEUE_DEPTH - 1;
    for (unsigned i = 0; i < FEBRUARY_SOFTBUS_QUEUE_DEPTH + 4; ++i) {
        SoftBusStub::instance().publish(100 + i, in, i);
    }
    EXPECT_GE(SoftBusStub::instance().drop_count(), 4u);
    EXPECT_EQ(SoftBusStub::instance().pending(), usable);
    const unsigned drained =
        SoftBusStub::instance().drain(100, [](const SoftBusMessage&) {});
    EXPECT_EQ(drained, usable);
    EXPECT_EQ(SoftBusStub::instance().pending(), 0u);
}

TEST_F(FebruaryReliabilityTest, SoftBusCodecEdgeFrames) {
    Intent in;
    in.type = IntentType::QueryStatus;
    in.confidence_x1000 = 1000;
    in.source_id = 0xDEADBEEF;
    in.param0 = -1;
    in.param1 = 0x7FFFFFFF;
    for (unsigned i = 0; i < 63; ++i) {
        in.text[i] = static_cast<char>('A' + (i % 26));
    }
    in.text[63] = '\0';

    uint8_t frame[kSoftBusFrameMax];
    const unsigned n = softbus_pack_intent(in, 42, 99999, frame, sizeof(frame));
    EXPECT_GT(n, 0u);
    EXPECT_LE(n, kSoftBusFrameMax);

    Intent out;
    uint32_t peer = 0, ts = 0;
    EXPECT_TRUE(softbus_unpack_intent(frame, n, out, peer, ts));
    EXPECT_EQ(out.type, IntentType::QueryStatus);
    EXPECT_EQ(out.confidence_x1000, 1000u);
    EXPECT_EQ(out.source_id, 0xDEADBEEFu);
    EXPECT_EQ(out.param0, -1);
    EXPECT_EQ(out.param1, 0x7FFFFFFF);
    EXPECT_EQ(peer, 42u);
    EXPECT_EQ(ts, 99999u);
    EXPECT_STREQ(out.text, in.text);

    EXPECT_FALSE(softbus_unpack_intent(frame, 2, out, peer, ts));  // short frame
    frame[0] ^= 0xFF;
    EXPECT_FALSE(softbus_unpack_intent(frame, n, out, peer, ts));  // bad magic
    frame[0] ^= 0xFF;

    uint8_t tiny[4];
    EXPECT_EQ(softbus_pack_intent(in, 1, 1, tiny, sizeof(tiny)), 0u);
}

TEST_F(FebruaryReliabilityTest, PeerTableReclaim) {
#if FEBRUARY_ENABLE_PEER_TABLE && FEBRUARY_ENABLE_SOFTBUS
    PeerTable::instance().clear();
    for (unsigned i = 1; i <= FEBRUARY_PEER_TABLE_SIZE + 2; ++i) {
        char net[16];
        std::snprintf(net, sizeof(net), "n%u", i);
        PeerSlot* s = PeerTable::instance().touch(i, net, i * 100);
        EXPECT_NE(s, nullptr) << "touch always succeeds via reclaim";
    }
    EXPECT_EQ(PeerTable::instance().count(),
              static_cast<unsigned>(FEBRUARY_PEER_TABLE_SIZE));
    PeerTable::instance().note_tx(99, 9999, true);
    PeerTable::instance().note_rx(99, 9999);
    PeerSlot* p99 = PeerTable::instance().find(99);
    ASSERT_NE(p99, nullptr);
    EXPECT_GE(p99->tx_ok, 1u);
    EXPECT_GE(p99->rx_ok, 1u);
#else
    GTEST_SKIP() << "PEER_TABLE / SOFTBUS disabled";
#endif
}

TEST_F(FebruaryReliabilityTest, CooldownTimeWrap) {
    CooldownGate cd(1000);
    EXPECT_TRUE(cd.try_fire(100));
    EXPECT_FALSE(cd.try_fire(500));
    EXPECT_TRUE(cd.try_fire(1100));
    EXPECT_TRUE(cd.try_fire(0xFFFFFFF0u));  // near wrap
    (void)cd.try_fire(10);
}

TEST_F(FebruaryReliabilityTest, PlannerRuleSwap) {
    static const PlanRule custom[] = {
        {IntentType::Greeting, false, 0, 1,
         {{ActionType::NotifyUser, 0, 0, "hi-custom"}}},
        {IntentType::None, false, 0, 0, {}},
    };
    Planner::instance().set_rules(custom);
    Intent in;
    in.type = IntentType::Greeting;
    in.confidence_x1000 = 900;
    UserContext ctx;
    Plan plan;
    unsigned n = Planner::instance().plan_for(in, ctx, plan);
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(plan.steps[0].type, ActionType::NotifyUser);
    EXPECT_NE(plan.steps[0].message, nullptr);
    if (plan.steps[0].message) {
        EXPECT_STREQ(plan.steps[0].message, "hi-custom");
    }
    Planner::instance().set_rules(nullptr);
    n = Planner::instance().plan_for(in, ctx, plan);
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(plan.steps[0].type, ActionType::Speak);
}

TEST_F(FebruaryReliabilityTest, ServiceLifecycleStress) {
    CapabilityHooks caps{};
    caps.on_speak = h_speak;
    caps.on_notify = h_notify;

    FebruaryService& svc = FebruaryService::instance();
    for (int round = 0; round < 20; ++round) {
        if (svc.state() != ServiceState::Stopped) {
            svc.stop();
        }
        SoftBus::instance().clear();
        EXPECT_TRUE(svc.start());
        svc.set_capability_hooks(caps);

        auto& core = FebruaryCore::instance();
        const uint32_t t = 1000u + static_cast<uint32_t>(round) * 100u;
        core.feed_text("status", t);
        svc.run_once(t);
        core.feed_battery(10, t + 10);
        svc.run_once(t + 10);

        svc.suspend();
        EXPECT_EQ(svc.run_once(t + 20), 0u);  // suspended idle
        svc.resume();
        svc.run_once(t + 30);
        svc.stop();
        EXPECT_EQ(svc.state(), ServiceState::Stopped);
    }
}

TEST_F(FebruaryReliabilityTest, RemoteYieldsToLocal) {
    FebruaryService& svc = FebruaryService::instance();
    if (svc.state() != ServiceState::Stopped) {
        svc.stop();
    }
    SoftBus::instance().clear();
    svc.start();

    CapabilityHooks caps{};
    caps.on_speak = h_speak;
    svc.set_capability_hooks(caps);

    auto& core = FebruaryCore::instance();
    const int speak0 = g_speak;

    Intent remote;
    remote.type = IntentType::Help;
    remote.confidence_x1000 = 900;
    SoftBusStub::instance().publish(55, remote, 5000);

    core.feed_text("status", 5000);
    EXPECT_TRUE(EventBus::instance().has_local_intent());
    svc.run_once(5000);
    EXPECT_EQ(SoftBusStub::instance().pending(), 0u);
    EXPECT_GT(g_speak, speak0);
    svc.stop();
}

TEST_F(FebruaryReliabilityTest, SoftBusSessionClose) {
    SoftBus& bus = SoftBus::instance();
    bus.clear();
    bus.start_server();
    EXPECT_TRUE(bus.register_peer(3, "net-3", 1));
    SoftBusSessionId sid = bus.ensure_session(3);
    EXPECT_GE(sid, 0);
    bus.close_peer(3);
    bus.on_session_closed(sid);
    sid = bus.ensure_session(3);
    EXPECT_GE(sid, 0);  // reopen after close
}

TEST_F(FebruaryReliabilityTest, CritEnterExitBalance) {
    const int e0 = g_crit_enter;
    const int x0 = g_crit_exit;
    for (int i = 0; i < 50; ++i) {
        EventBus::instance().publish(Event{});
    }
    EventBus::instance().process(50);
    for (int i = 0; i < 30; ++i) {
        Intent in;
        in.type = IntentType::Help;
        SoftBusStub::instance().publish(1, in, static_cast<uint32_t>(i));
    }
    SoftBusStub::instance().drain(30, [](const SoftBusMessage&) {});
    EXPECT_EQ(g_crit_enter - e0, g_crit_exit - x0);
    EXPECT_EQ(g_crit_depth, 0);
    EXPECT_FALSE(g_crit_underflow);
}

TEST_F(FebruaryReliabilityTest, BoardBindStartsService) {
    FebruaryService::instance().stop();
    SoftBus::instance().clear();
    CapabilityHooks caps{};
    caps.on_speak = h_speak;
    BoardBindArgs a{};
    a.crit_enter = crit_enter;
    a.crit_exit = crit_exit;
    a.caps = &caps;
    EXPECT_TRUE(board_bind_start(a));
    EXPECT_EQ(FebruaryService::instance().state(), ServiceState::Running);
    FebruaryService::instance().stop();
}

TEST_F(FebruaryReliabilityTest, MultiPeerPublishStress) {
    SoftBus& bus = SoftBus::instance();
    bus.clear();

    // Bind a transport so outbound publish has a real TX path.
    SoftBusTransportOps ops;
    ops.create_server = rel_create;
    ops.open_session = rel_open;
    ops.send_bytes = rel_send;
    bus.bind_transport(ops);
    bus.start_server();

    for (uint32_t p = 1; p <= FEBRUARY_SOFTBUS_MAX_SESSIONS; ++p) {
        char net[16];
        std::snprintf(net, sizeof(net), "peer-%u", p);
        EXPECT_TRUE(bus.register_peer(p, net, p));
    }
    EXPECT_FALSE(bus.register_peer(99, "overflow", 0));  // session table full

    Intent in;
    in.type = IntentType::Greeting;
    in.confidence_x1000 = 800;

    const uint32_t tx0 = bus.tx_count();
    for (uint32_t p = 1; p <= FEBRUARY_SOFTBUS_MAX_SESSIONS; ++p) {
        // loopback=false: real outbound. Must report success now that a
        // transport is bound, and must NOT be enqueued into the local inbox.
        EXPECT_TRUE(bus.publish_intent(p, in, 1000 + p, false));
    }
    EXPECT_EQ(bus.tx_count() - tx0,
              static_cast<uint32_t>(FEBRUARY_SOFTBUS_MAX_SESSIONS));

    // A real outbound must not pollute the local inbox.
    EXPECT_EQ(bus.pending(), 0u);

    // The inbox is filled by actual RX, so simulate a peer echoing a frame
    // back to us on the session we opened.
    uint8_t frame[kSoftBusFrameMax];
    const unsigned n = softbus_pack_intent(in, 1, 2000, frame, sizeof(frame));
    EXPECT_GT(n, 0u);
    bus.on_bytes_received(900, frame, n);
    EXPECT_GT(bus.pending(), 0u);
    bus.drain(100, [](const SoftBusMessage&) {});
}