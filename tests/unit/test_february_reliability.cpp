/**
 * February reliability / stress host test (Phase 2.2)
 *
 * Covers: EventBus overflow, SoftBus inbox drop-oldest, PeerTable reclaim,
 * Crit reentrancy balance, planner table swap, remote yield, time wrap,
 * codec edge frames, multi-run service lifecycle.
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror -I. \
 *       -o /tmp/test_rel tests/unit/test_february_reliability.cpp
 *   /tmp/test_rel
 */
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

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>

using namespace aurora::february;

static int g_crit_depth = 0;
static int g_crit_enter = 0;
static int g_crit_exit  = 0;
static int g_speak = 0;
static int g_notify = 0;
static int g_failures = 0;

#define REL_CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        ++g_failures; \
    } \
} while (0)

static void crit_enter(void*) {
    ++g_crit_enter;
    ++g_crit_depth;
}
static void crit_exit(void*) {
    ++g_crit_exit;
    --g_crit_depth;
    REL_CHECK(g_crit_depth >= 0, "crit depth underflow");
}

static void h_speak(const char*, void*) { ++g_speak; }
static void h_notify(const char*, void*) { ++g_notify; }

static void test_string_util() {
    std::printf("[1] string_util\n");
    REL_CHECK(contains_ci("Hey February", "february"), "contains_ci match");
    REL_CHECK(!contains_ci("abc", "xyz"), "contains_ci miss");
    char buf[4];
    REL_CHECK(copy_cstr(buf, sizeof(buf), "abcdef") == 3, "copy trunc");
    REL_CHECK(std::strcmp(buf, "abc") == 0, "copy content");
    REL_CHECK(copy_cstr(buf, sizeof(buf), nullptr) == 0, "copy null");
}

static void test_event_bus_overflow() {
    std::printf("[2] EventBus overflow drop-oldest\n");
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
    REL_CHECK(EventBus::instance().drop_count() >= 5, "drops recorded");
    unsigned n = EventBus::instance().process(depth + 10);
    REL_CHECK(n == usable, "queue holds at most usable (=depth-1)");
    REL_CHECK(EventBus::instance().process(1) == 0, "empty after drain");
}

static void test_event_bus_has_local() {
    std::printf("[3] EventBus has_local_intent\n");
    EventBus::instance().clear();
    REL_CHECK(!EventBus::instance().has_local_intent(), "empty no local");
    Event ev;
    ev.type = EventType::SensorUpdate;
    EventBus::instance().publish(ev);
    REL_CHECK(!EventBus::instance().has_local_intent(), "sensor not local intent");
    ev.type = EventType::IntentDetected;
    EventBus::instance().publish(ev);
    REL_CHECK(EventBus::instance().has_local_intent(), "intent present");
    EventBus::instance().process(16);
    REL_CHECK(!EventBus::instance().has_local_intent(), "cleared after process");
}

static void test_softbus_inbox_overflow() {
    std::printf("[4] SoftBusStub overflow\n");
    SoftBusStub::instance().clear();
    Intent in;
    in.type = IntentType::Help;
    in.confidence_x1000 = 900;
    const unsigned usable = FEBRUARY_SOFTBUS_QUEUE_DEPTH - 1;
    for (unsigned i = 0; i < FEBRUARY_SOFTBUS_QUEUE_DEPTH + 4; ++i) {
        SoftBusStub::instance().publish(100 + i, in, i);
    }
    REL_CHECK(SoftBusStub::instance().drop_count() >= 4, "softbus drops");
    REL_CHECK(SoftBusStub::instance().pending() == usable, "pending == usable");
    unsigned drained = SoftBusStub::instance().drain(100, [](const SoftBusMessage&) {});
    REL_CHECK(drained == usable, "drain all");
    REL_CHECK(SoftBusStub::instance().pending() == 0, "empty");
}

static void test_codec_edges() {
    std::printf("[5] SoftBus codec edges\n");
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
    unsigned n = softbus_pack_intent(in, 42, 99999, frame, sizeof(frame));
    REL_CHECK(n > 0, "pack ok");
    REL_CHECK(n <= kSoftBusFrameMax, "pack size");

    Intent out;
    uint32_t peer = 0, ts = 0;
    REL_CHECK(softbus_unpack_intent(frame, n, out, peer, ts), "unpack ok");
    REL_CHECK(out.type == IntentType::QueryStatus, "type");
    REL_CHECK(out.confidence_x1000 == 1000, "conf");
    REL_CHECK(out.source_id == 0xDEADBEEF, "source");
    REL_CHECK(out.param0 == -1, "param0");
    REL_CHECK(out.param1 == 0x7FFFFFFF, "param1");
    REL_CHECK(peer == 42, "peer");
    REL_CHECK(ts == 99999, "ts");
    REL_CHECK(std::strcmp(out.text, in.text) == 0, "text roundtrip");

    REL_CHECK(!softbus_unpack_intent(frame, 2, out, peer, ts), "short frame");
    frame[0] ^= 0xFF;
    REL_CHECK(!softbus_unpack_intent(frame, n, out, peer, ts), "bad magic");
    frame[0] ^= 0xFF;

    uint8_t tiny[4];
    REL_CHECK(softbus_pack_intent(in, 1, 1, tiny, sizeof(tiny)) == 0, "pack tiny fail");
}

static void test_peer_table_reclaim() {
    std::printf("[6] PeerTable reclaim\n");
#if FEBRUARY_ENABLE_PEER_TABLE && FEBRUARY_ENABLE_SOFTBUS
    PeerTable::instance().clear();
    for (unsigned i = 1; i <= FEBRUARY_PEER_TABLE_SIZE + 2; ++i) {
        char net[16];
        std::snprintf(net, sizeof(net), "n%u", i);
        PeerSlot* s = PeerTable::instance().touch(i, net, i * 100);
        REL_CHECK(s != nullptr, "touch always succeeds via reclaim");
    }
    REL_CHECK(PeerTable::instance().count() == FEBRUARY_PEER_TABLE_SIZE,
              "count capped");
    PeerTable::instance().note_tx(99, 9999, true);
    PeerTable::instance().note_rx(99, 9999);
    PeerSlot* p99 = PeerTable::instance().find(99);
    REL_CHECK(p99 && p99->tx_ok >= 1 && p99->rx_ok >= 1, "tx/rx counters");
#else
    std::printf("  (skipped: PEER_TABLE disabled)\n");
#endif
}

static void test_cooldown_wrap() {
    std::printf("[7] Cooldown time wrap\n");
    CooldownGate cd(1000);
    REL_CHECK(cd.try_fire(100), "first fire");
    REL_CHECK(!cd.try_fire(500), "cooldown active");
    REL_CHECK(cd.try_fire(1100), "after cooldown");
    REL_CHECK(cd.try_fire(0xFFFFFFF0u), "near wrap fire");
    (void)cd.try_fire(10);
}

static void test_planner_swap() {
    std::printf("[8] Planner set_rules\n");
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
    REL_CHECK(n == 1, "custom one step");
    REL_CHECK(plan.steps[0].type == ActionType::NotifyUser, "custom type");
    REL_CHECK(plan.steps[0].message &&
              std::strcmp(plan.steps[0].message, "hi-custom") == 0,
              "custom msg");
    Planner::instance().set_rules(nullptr);
    n = Planner::instance().plan_for(in, ctx, plan);
    REL_CHECK(n == 1 && plan.steps[0].type == ActionType::Speak,
              "default restored speak");
}

static void test_service_lifecycle_stress() {
    std::printf("[9] Service lifecycle x20\n");
    CapabilityHooks caps{};
    caps.on_speak = h_speak;
    caps.on_notify = h_notify;

    FebruaryService& svc = FebruaryService::instance();
    for (int round = 0; round < 20; ++round) {
        if (svc.state() != ServiceState::Stopped) {
            svc.stop();
        }
        SoftBus::instance().clear();
        REL_CHECK(svc.start(), "start");
        svc.set_capability_hooks(caps);

        auto& core = FebruaryCore::instance();
        uint32_t t = 1000u + static_cast<uint32_t>(round) * 100u;
        core.feed_text("status", t);
        svc.run_once(t);
        core.feed_battery(10, t + 10);
        svc.run_once(t + 10);

        svc.suspend();
        REL_CHECK(svc.run_once(t + 20) == 0, "suspended idle");
        svc.resume();
        svc.run_once(t + 30);
        svc.stop();
        REL_CHECK(svc.state() == ServiceState::Stopped, "stopped");
    }
}

static void test_remote_yield() {
    std::printf("[10] Remote yield to local\n");
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
    REL_CHECK(EventBus::instance().has_local_intent(), "local pending");
    svc.run_once(5000);
    REL_CHECK(SoftBusStub::instance().pending() == 0, "remote eventually drained");
    REL_CHECK(g_speak > speak0, "spoke something");
    svc.stop();
}

static void test_softbus_session_close() {
    std::printf("[11] SoftBus session close\n");
    SoftBus& bus = SoftBus::instance();
    bus.clear();
    bus.start_server();
    REL_CHECK(bus.register_peer(3, "net-3", 1), "reg peer");
    SoftBusSessionId sid = bus.ensure_session(3);
    REL_CHECK(sid >= 0, "session open");
    bus.close_peer(3);
    bus.on_session_closed(sid);
    sid = bus.ensure_session(3);
    REL_CHECK(sid >= 0, "reopen after close");
}

static void test_crit_balance() {
    std::printf("[12] Crit enter/exit balance\n");
    const int e0 = g_crit_enter;
    const int x0 = g_crit_exit;
    for (int i = 0; i < 50; ++i) {
        EventBus::instance().publish(Event{});
    }
    EventBus::instance().process(50);
    for (int i = 0; i < 30; ++i) {
        Intent in;
        in.type = IntentType::Help;
        SoftBusStub::instance().publish(1, in, i);
    }
    SoftBusStub::instance().drain(30, [](const SoftBusMessage&) {});
    REL_CHECK(g_crit_enter - e0 == g_crit_exit - x0, "enter==exit");
    REL_CHECK(g_crit_depth == 0, "depth zero");
}

static void test_board_bind() {
    std::printf("[13] board_bind_start\n");
    FebruaryService::instance().stop();
    SoftBus::instance().clear();
    CapabilityHooks caps{};
    caps.on_speak = h_speak;
    BoardBindArgs a{};
    a.crit_enter = crit_enter;
    a.crit_exit = crit_exit;
    a.caps = &caps;
    REL_CHECK(board_bind_start(a), "board bind");
    REL_CHECK(FebruaryService::instance().state() == ServiceState::Running,
              "running");
    FebruaryService::instance().stop();
}

static void test_multi_peer_publish() {
    std::printf("[14] Multi-peer publish stress\n");
    SoftBus& bus = SoftBus::instance();
    bus.clear();
    bus.start_server();
    for (uint32_t p = 1; p <= FEBRUARY_SOFTBUS_MAX_SESSIONS; ++p) {
        char net[16];
        std::snprintf(net, sizeof(net), "peer-%u", p);
        REL_CHECK(bus.register_peer(p, net, p), "reg");
    }
    REL_CHECK(!bus.register_peer(99, "overflow", 0), "session table full");
    Intent in;
    in.type = IntentType::Greeting;
    in.confidence_x1000 = 800;
    for (uint32_t p = 1; p <= FEBRUARY_SOFTBUS_MAX_SESSIONS; ++p) {
        REL_CHECK(bus.publish_intent(p, in, 1000 + p, false), "publish");
    }
    unsigned pending = bus.pending();
    REL_CHECK(pending > 0, "inbox has msgs");
    bus.drain(100, [](const SoftBusMessage&) {});
}

int main() {
    std::printf("=== February RELIABILITY suite ===\n");
    FebruaryCrit::set(crit_enter, crit_exit);

    test_string_util();
    test_event_bus_overflow();
    test_event_bus_has_local();
    test_softbus_inbox_overflow();
    test_codec_edges();
    test_peer_table_reclaim();
    test_cooldown_wrap();
    test_planner_swap();
    test_service_lifecycle_stress();
    test_remote_yield();
    test_softbus_session_close();
    test_crit_balance();
    test_board_bind();
    test_multi_peer_publish();

    FebruaryCrit::set(nullptr, nullptr);

    if (g_failures == 0) {
        std::printf("=== ALL RELIABILITY CHECKS PASSED (speak=%d notify=%d "
                    "crit=%d/%d) ===\n",
                    g_speak, g_notify, g_crit_enter, g_crit_exit);
        return 0;
    }
    std::printf("=== RELIABILITY FAILURES: %d ===\n", g_failures);
    return 1;
}
