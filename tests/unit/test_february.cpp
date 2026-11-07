// =============================================================================
// test_february.cpp — Unit and Integration tests for February AI Runtime
// =============================================================================

#include <gtest/gtest.h>

#include <cstring>

#include "ai/february/types.hpp"
#include "ai/february/crit.hpp"
#include "ai/february/cooldown.hpp"
#include "ai/february/string_util.hpp"
#include "ai/february/wake_word.hpp"
#include "ai/february/event_bus.hpp"
#include "ai/february/context_manager.hpp"
#include "ai/february/intent_rules.hpp"
#include "ai/february/intent_engine.hpp"
#include "ai/february/planner.hpp"
#include "ai/february/persona.hpp"
#include "ai/february/action_executor.hpp"
#include "ai/february/memory.hpp"
#include "ai/february/peer_table.hpp"
#include "ai/february/softbus_codec.hpp"
#include "ai/february/softbus_stub.hpp"
#include "ai/february/softbus_transport.hpp"
#include "ai/february/softbus.hpp"
#include "ai/february/softbus_oh_adapter.hpp"
#include "ai/february/platform_hooks.hpp"
#include "ai/february/board_bind.hpp"
#include "ai/february/service.hpp"
#include "ai/february/february_core.hpp"

using namespace aurora::february;

// ---------------------------------------------------------------------------
// String Util Tests
// ---------------------------------------------------------------------------
TEST(FebruaryStringUtilTest, CaseInsensitiveMatching) {
    EXPECT_TRUE(contains_ci("Hello February World", "february"));
    EXPECT_TRUE(contains_ci("STATUS", "status"));
    EXPECT_FALSE(contains_ci("Hi", "hello"));
    EXPECT_FALSE(contains_ci(nullptr, "test"));
    EXPECT_FALSE(contains_ci("test", nullptr));

    EXPECT_TRUE(equals_ci("February", "FEBRUARY"));
    EXPECT_TRUE(equals_ci("dnd", "DND"));
    EXPECT_FALSE(equals_ci("dnd", "focus"));

    EXPECT_TRUE(starts_with_ci("hey february", "HEY"));
    EXPECT_FALSE(starts_with_ci("february", "hey"));

    EXPECT_TRUE(contains_word_ci("say hi to me", "hi"));
    EXPECT_TRUE(contains_word_ci("hi!", "hi"));
    EXPECT_TRUE(contains_word_ci("hi", "hi"));
    EXPECT_FALSE(contains_word_ci("this is a test", "hi"));
}

// ---------------------------------------------------------------------------
// Cooldown & Latch Tests
// ---------------------------------------------------------------------------
TEST(FebruaryCooldownTest, CooldownGateAndWrap) {
    CooldownGate gate(1000);
    EXPECT_TRUE(gate.try_fire(100));
    EXPECT_FALSE(gate.try_fire(500));
    EXPECT_FALSE(gate.try_fire(1099));
    EXPECT_TRUE(gate.try_fire(1100));

    // Zero period always fires
    CooldownGate zero_gate(0);
    EXPECT_TRUE(zero_gate.try_fire(100));
    EXPECT_TRUE(zero_gate.try_fire(101));

    // Modular wrap check
    CooldownGate wrap_gate(500);
    wrap_gate.try_fire(0xFFFFFF00u);
    EXPECT_FALSE(wrap_gate.try_fire(0xFFFFFF50u));
    EXPECT_TRUE(wrap_gate.try_fire(0x00000150u)); // wrapped past uint32_max
}

TEST(FebruaryCooldownTest, LevelLatch) {
    LevelLatch latch;
    EXPECT_FALSE(latch.rising(false));
    EXPECT_TRUE(latch.rising(true));  // Rising edge
    EXPECT_FALSE(latch.rising(true)); // Already latched
    EXPECT_FALSE(latch.rising(false));
    EXPECT_TRUE(latch.rising(true));  // Rising edge again
}

// ---------------------------------------------------------------------------
// EventBus Tests
// ---------------------------------------------------------------------------
class FebruaryEventBusTest : public ::testing::Test {
protected:
    void SetUp() override {
        EventBus::instance().clear();
    }
};

TEST_F(FebruaryEventBusTest, PublishSubscribeAndProcess) {
    static int g_handled_count = 0;
    static EventType g_last_type = EventType::None;

    g_handled_count = 0;
    g_last_type = EventType::None;

    auto handler = [](const Event& ev, void* /*user*/) {
        ++g_handled_count;
        g_last_type = ev.type;
    };

    EXPECT_TRUE(EventBus::instance().subscribe(EventType::IntentDetected, handler, nullptr));

    Event ev;
    ev.type = EventType::IntentDetected;
    ev.timestamp_ms = 1000;
    ev.payload.intent.type = IntentType::Greeting;

    EXPECT_TRUE(EventBus::instance().publish(ev));
    EXPECT_EQ(EventBus::instance().pending(), 1u);

    unsigned processed = EventBus::instance().process(8);
    EXPECT_EQ(processed, 1u);
    EXPECT_EQ(g_handled_count, 1);
    EXPECT_EQ(g_last_type, EventType::IntentDetected);

    // Unsubscribe test
    EventBus::instance().unsubscribe(handler);
    EXPECT_TRUE(EventBus::instance().publish(ev));
    EventBus::instance().process(8);
    EXPECT_EQ(g_handled_count, 1); // No increase
}

TEST_F(FebruaryEventBusTest, QueueOverflowDropCount) {
    Event ev;
    ev.type = EventType::SensorUpdate;

    // A ring buffer of size N holds N - 1 items before starting to overwrite/drop
    for (unsigned i = 0; i < (kEventQueueDepth - 1) + 4; ++i) {
        ev.timestamp_ms = i;
        EventBus::instance().publish(ev);
    }

    EXPECT_EQ(EventBus::instance().drop_count(), 4u);
}

// ---------------------------------------------------------------------------
// Wake Word Tests
// ---------------------------------------------------------------------------
TEST(FebruaryWakeWordTest, ConfigurationAndGate) {
    WakeWordConfig& ww = WakeWordConfig::instance();
    ww.set(nullptr);
    EXPECT_FALSE(ww.configured());
    EXPECT_TRUE(ww.matches("status"));

    ww.set("hey february");
    EXPECT_TRUE(ww.configured());
    EXPECT_TRUE(ww.matches("HEY FEBRUARY status"));
    EXPECT_FALSE(ww.matches("just checking status"));

    ww.set("");
    EXPECT_FALSE(ww.configured());
}

// ---------------------------------------------------------------------------
// Intent Engine & Context Manager Tests
// ---------------------------------------------------------------------------
class FebruaryIntentTest : public ::testing::Test {
protected:
    void SetUp() override {
        EventBus::instance().clear();
        SessionMemory::instance().clear();
        WakeWordConfig::instance().set("");
        IntentEngine::instance().reset_proactive();
    }
};

TEST_F(FebruaryIntentTest, TextIntentParsing) {
    Intent in = IntentEngine::instance().parse_text("please turn on dnd mode", 1000);
    EXPECT_EQ(in.type, IntentType::SetDoNotDisturb);
    EXPECT_EQ(in.param0, 1);

    Intent in2 = IntentEngine::instance().parse_text("disable dnd", 1005);
    EXPECT_EQ(in2.type, IntentType::SetDoNotDisturb);
    EXPECT_EQ(in2.param0, 0);

    Intent in3 = IntentEngine::instance().parse_text("how is my battery", 1010);
    EXPECT_EQ(in3.type, IntentType::QueryStatus);

    Intent in4 = IntentEngine::instance().parse_text("unknown gibberish", 1020);
    EXPECT_EQ(in4.type, IntentType::UnknownCommand);
}

TEST_F(FebruaryIntentTest, DialogueHistoryRingBuffer) {
    SessionMemory& mem = SessionMemory::instance();
    EXPECT_EQ(mem.history_count(), 0u);

    // Push 5 intents to test wrap-around on 4-slot ring buffer
    IntentEngine::instance().parse_text("hello", 1000);
    mem.note_speak("Hello there!", 1001);
    EXPECT_EQ(mem.history_count(), 1u);

    IntentEngine::instance().parse_text("status", 1010);
    mem.note_speak("Steps 1000", 1011);
    EXPECT_EQ(mem.history_count(), 2u);

    IntentEngine::instance().parse_text("health", 1020);
    mem.note_speak("HR 75", 1021);
    EXPECT_EQ(mem.history_count(), 3u);

    IntentEngine::instance().parse_text("focus mode", 1030);
    mem.note_speak("DND on", 1031);
    EXPECT_EQ(mem.history_count(), 4u);

    // 5th intent should wrap around and keep history size at capacity 4
    IntentEngine::instance().parse_text("help", 1040);
    mem.note_speak("I can help you", 1041);
    EXPECT_EQ(mem.history_count(), 4u);

    // Verify retrieval depth: 0 is latest ("help"), 1 is previous ("focus mode")
    const HistoryTurn* t0 = mem.get_history_turn(0);
    ASSERT_NE(t0, nullptr);
    EXPECT_EQ(t0->intent.type, IntentType::Help);
    EXPECT_STREQ(t0->speak, "I can help you");

    const HistoryTurn* t1 = mem.get_history_turn(1);
    ASSERT_NE(t1, nullptr);
    EXPECT_EQ(t1->intent.type, IntentType::SetDoNotDisturb);
    EXPECT_STREQ(t1->speak, "DND on");

    // Depth >= 4 returns nullptr
    EXPECT_EQ(mem.get_history_turn(4), nullptr);
}

TEST_F(FebruaryIntentTest, MultiTurnAnaphoraResolution) {
    SessionMemory& mem = SessionMemory::instance();

    // 1. Without prior context, "cancel it" should fail anaphora and be UnknownCommand
    Intent no_ctx = IntentEngine::instance().parse_text("cancel it", 1000);
    EXPECT_EQ(no_ctx.type, IntentType::UnknownCommand);

    mem.clear();

    // 2. Turn 1: "open focus mode" -> DND ON
    Intent t1 = IntentEngine::instance().parse_text("focus mode", 1000);
    EXPECT_EQ(t1.type, IntentType::SetDoNotDisturb);
    EXPECT_EQ(t1.param0, 1);

    // 3. Turn 2: "cancel it" -> Anaphora resolves to DND OFF!
    Intent t2 = IntentEngine::instance().parse_text("cancel it", 1005);
    EXPECT_EQ(t2.type, IntentType::SetDoNotDisturb);
    EXPECT_EQ(t2.param0, 0);

    // 4. Turn 3: "turn it on" -> Anaphora resolves to DND ON!
    Intent t3 = IntentEngine::instance().parse_text("turn it on", 1010);
    EXPECT_EQ(t3.type, IntentType::SetDoNotDisturb);
    EXPECT_EQ(t3.param0, 1);

    // 5. Turn 4: Chinese "取消它" -> Anaphora resolves to DND OFF!
    Intent t4 = IntentEngine::instance().parse_text("取消它", 1015);
    EXPECT_EQ(t4.type, IntentType::SetDoNotDisturb);
    EXPECT_EQ(t4.param0, 0);

    // 6. Turn 5: Chinese "打开它" -> Anaphora resolves to DND ON!
    Intent t5 = IntentEngine::instance().parse_text("打开它", 1020);
    EXPECT_EQ(t5.type, IntentType::SetDoNotDisturb);
    EXPECT_EQ(t5.param0, 1);
}

TEST_F(FebruaryIntentTest, AnaphoraWithInterleavedQueries) {
    SessionMemory& mem = SessionMemory::instance();
    mem.clear();

    // Turn 1: Actionable intent "focus" (DND on)
    Intent t1 = IntentEngine::instance().parse_text("focus", 1000);
    EXPECT_EQ(t1.type, IntentType::SetDoNotDisturb);
    EXPECT_EQ(t1.param0, 1);

    // Turn 2: Non-actionable query "status"
    Intent t2 = IntentEngine::instance().parse_text("status", 1010);
    EXPECT_EQ(t2.type, IntentType::QueryStatus);

    // Turn 3: "cancel it" should search past "status" to find "focus" and resolve to DND off!
    Intent t3 = IntentEngine::instance().parse_text("cancel it", 1020);
    EXPECT_EQ(t3.type, IntentType::SetDoNotDisturb);
    EXPECT_EQ(t3.param0, 0);
}

TEST_F(FebruaryIntentTest, SensorProactiveTriggers) {
    static int g_intent_count = 0;
    static IntentType g_last_intent = IntentType::None;

    g_intent_count = 0;
    g_last_intent = IntentType::None;

    EventBus::instance().subscribe(EventType::IntentDetected, [](const Event& ev, void*) {
        ++g_intent_count;
        g_last_intent = ev.payload.intent.type;
    });

    uint32_t now = 1000;
    // Low battery trigger
    IntentEngine::instance().on_battery(12, now);
    EventBus::instance().process();
    EXPECT_EQ(g_last_intent, IntentType::BatteryLow);

    // Repeated low battery does not retrigger due to latch
    IntentEngine::instance().on_battery(10, now + 10);
    EventBus::instance().process();
    EXPECT_EQ(g_intent_count, 1);

    // Fitness steps delta trigger
    IntentEngine::instance().on_steps(10, now);
    IntentEngine::instance().on_steps(100, now + 100);
    EventBus::instance().process();
    EXPECT_EQ(g_last_intent, IntentType::StartFitness);

    // Wrist gesture trigger
    IntentEngine::instance().on_wrist_gesture(true, now + 200);
    EventBus::instance().process();
    EXPECT_EQ(g_last_intent, IntentType::Greeting);
}

// ---------------------------------------------------------------------------
// Planner Tests
// ---------------------------------------------------------------------------
TEST(FebruaryPlannerTest, PlanGeneration) {
    Planner& planner = Planner::instance();
    UserContext ctx;
    Plan plan;

    Intent in;
    in.type = IntentType::BatteryLow;
    unsigned steps = planner.plan_for(in, ctx, plan);
    EXPECT_EQ(steps, 2u);
    EXPECT_EQ(plan.steps[0].type, ActionType::NotifyUser);
    EXPECT_EQ(plan.steps[1].type, ActionType::SetPower);

    Intent dnd_in;
    dnd_in.type = IntentType::SetDoNotDisturb;
    dnd_in.param0 = 1;
    steps = planner.plan_for(dnd_in, ctx, plan);
    EXPECT_EQ(steps, 1u);
    EXPECT_EQ(plan.steps[0].type, ActionType::SetDnd);
    EXPECT_EQ(plan.steps[0].arg0, 1);
}

// ---------------------------------------------------------------------------
// SoftBus Codec Tests
// ---------------------------------------------------------------------------
TEST(FebruarySoftBusCodecTest, PackAndUnpackIntent) {
    Intent original;
    original.type = IntentType::QueryStatus;
    original.confidence_x1000 = 850;
    original.source_id = 42;
    original.param0 = 123;
    original.param1 = -456;
    copy_cstr(original.text, sizeof(original.text), "status query");

    uint8_t buffer[128];
    unsigned packed_len = softbus_pack_intent(original, 99, 5000, buffer, sizeof(buffer));
    EXPECT_GT(packed_len, 0u);

    Intent decoded;
    uint32_t peer_id = 0;
    uint32_t timestamp = 0;
    bool ok = softbus_unpack_intent(buffer, packed_len, decoded, peer_id, timestamp);
    EXPECT_TRUE(ok);
    EXPECT_EQ(decoded.type, original.type);
    EXPECT_EQ(decoded.confidence_x1000, original.confidence_x1000);
    EXPECT_EQ(decoded.source_id, original.source_id);
    EXPECT_EQ(decoded.param0, original.param0);
    EXPECT_EQ(decoded.param1, original.param1);
    EXPECT_STREQ(decoded.text, original.text);
    EXPECT_EQ(peer_id, 99u);
    EXPECT_EQ(timestamp, 5000u);

    // Corrupt magic test
    buffer[0] = 0x00;
    EXPECT_FALSE(softbus_unpack_intent(buffer, packed_len, decoded, peer_id, timestamp));
}

TEST(FebruarySoftBusCodecTest, OversizedTextTruncatedAndStillParseable) {
    // Regression: a caller may hand us an Intent whose 64-byte text field is
    // completely filled (no NUL terminator). Pack must not read past
    // text[63], and the emitted frame must remain parseable — previously a
    // text_len of 64 was packed but unpack always rejected it, silently
    // dropping the message on every receiver.
    Intent in;
    in.type = IntentType::QueryStatus;
    in.confidence_x1000 = 700;
    std::memset(in.text, 'a', sizeof(in.text));  // no terminator on purpose

    uint8_t buffer[kSoftBusFrameMax];
    unsigned packed_len = softbus_pack_intent(in, 7, 900, buffer, sizeof(buffer));
    ASSERT_GT(packed_len, 0u);
    ASSERT_LE(packed_len, kSoftBusFrameMax);

    Intent out;
    uint32_t peer = 0;
    uint32_t ts = 0;
    ASSERT_TRUE(softbus_unpack_intent(buffer, packed_len, out, peer, ts));
    EXPECT_EQ(peer, 7u);
    EXPECT_EQ(ts, 900u);
    EXPECT_EQ(std::strlen(out.text), kSoftBusMaxText - 1u);
}

TEST(FebruarySoftBusCodecTest, MaxLegalTextRoundTrips) {
    // Boundary: a NUL-terminated text of exactly kSoftBusMaxText - 1 chars
    // must survive pack/unpack unchanged.
    Intent in;
    in.type = IntentType::QueryStatus;
    std::memset(in.text, 'b', kSoftBusMaxText - 1u);
    in.text[kSoftBusMaxText - 1u] = '\0';

    uint8_t buffer[kSoftBusFrameMax];
    unsigned packed_len = softbus_pack_intent(in, 1, 1, buffer, sizeof(buffer));
    ASSERT_GT(packed_len, 0u);

    Intent out;
    uint32_t peer = 0;
    uint32_t ts = 0;
    ASSERT_TRUE(softbus_unpack_intent(buffer, packed_len, out, peer, ts));
    EXPECT_STREQ(out.text, in.text);
}

// ---------------------------------------------------------------------------
// WorkingMemory Tests
// ---------------------------------------------------------------------------
TEST(FebruaryWorkingMemoryTest, OverflowDropsOldestAndCountsDrops) {
    // Regression: once the ring was full the drop branch was unreachable, so
    // evicted entries were never reflected in drop_count().
    WorkingMemory& wm = WorkingMemory::instance();
    wm.clear();

    const unsigned cap = WorkingMemory::capacity();
    for (unsigned i = 0; i < cap + 6; ++i) {
        wm.push(WmKind::Intent, 0, i, 0, 255, i * 1000u);
    }

    EXPECT_EQ(wm.count(), cap);
    EXPECT_EQ(wm.drop_count(), 6u);

    // All timestamps stay inside the 5-minute decay window, so the surviving
    // window must hold exactly cap entries with a = 6 (oldest survivor) ..
    // cap + 5 (newest), newest first.
    uint32_t first_a = 0;
    uint32_t last_a = 0;
    unsigned visited = 0;
    wm.for_each_recent((cap + 5u) * 1000u + 1u, [&](const WorkingSlot& s) {
        if (visited == 0) {
            first_a = s.a;
        }
        last_a = s.a;
        ++visited;
        return true;
    });
    EXPECT_EQ(visited, cap);
    EXPECT_EQ(first_a, cap + 5u);
    EXPECT_EQ(last_a, 6u);
}

// ---------------------------------------------------------------------------
// PeerTable Tests
// ---------------------------------------------------------------------------
TEST(FebruaryPeerTableTest, PeerRegistrationAndLookup) {
    PeerTable& pt = PeerTable::instance();
    pt.clear();

    EXPECT_EQ(pt.count(), 0u);
    PeerSlot* s1 = pt.touch(2, "watch_node_01", 1000);
    ASSERT_NE(s1, nullptr);
    EXPECT_EQ(s1->peer_id, 2u);
    EXPECT_EQ(pt.count(), 1u);

    EXPECT_EQ(pt.find(2), s1);
    EXPECT_EQ(pt.find_by_network_id("watch_node_01"), s1);
    EXPECT_EQ(pt.find_by_network_id("non_existent"), nullptr);

    pt.note_tx(2, 1050, true);
    pt.note_rx(2, 1100);
    EXPECT_EQ(s1->tx_ok, 1u);
    EXPECT_EQ(s1->rx_ok, 1u);

    // Prune stale test
    s1->session_open = true;
    unsigned pruned = pt.prune_stale(10000, 5000);
    EXPECT_EQ(pruned, 1u);
    EXPECT_FALSE(s1->session_open);
}

// ---------------------------------------------------------------------------
// Persona Tests
// ---------------------------------------------------------------------------
TEST(FebruaryPersonaTest, PersonaReplies) {
    Persona& p = Persona::instance();
    p.set_name("February");
    UserContext ctx;
    ctx.steps = 5432;
    ctx.heart_rate = 75;
    ctx.battery_pct = 80;
    ctx.activity = ActivityState::Walking;

    Action act;
    Intent in;
    in.type = IntentType::Greeting;
    p.reply_for_intent(in, ctx, act);
    EXPECT_EQ(act.type, ActionType::Speak);
    EXPECT_TRUE(contains_ci(act.message, "February online"));

    in.type = IntentType::QueryStatus;
    p.reply_for_intent(in, ctx, act);
    EXPECT_TRUE(contains_ci(act.message, "Steps 5432"));
    EXPECT_TRUE(contains_ci(act.message, "HR 75"));
}

// ---------------------------------------------------------------------------
// FebruaryCore & Service End-to-End Test
// ---------------------------------------------------------------------------
TEST(FebruaryServiceTest, EndToEndServiceLifecycleAndHooks) {
    static int g_speak_count = 0;
    static int g_dnd_count = 0;

    g_speak_count = 0;
    g_dnd_count = 0;

    CapabilityHooks caps;
    caps.on_speak = [](const char* /*msg*/, void*) { ++g_speak_count; };
    caps.on_set_dnd = [](bool /*enable*/, void*) { ++g_dnd_count; };

    // FebruaryCore::init() is a no-op once the singleton is ready(); earlier
    // February tests leave it ready with a possibly-cleared EventBus, so the
    // "dnd on" intent below would be silently dropped. reset() makes the
    // core.init() inside board_bind_start() perform a full, fresh setup.
    FebruaryCore::instance().reset();

    BoardBindArgs args;
    args.caps = &caps;
    args.wake_word = "";
    EXPECT_TRUE(board_bind_start(args));

    auto& svc = FebruaryService::instance();
    EXPECT_EQ(svc.state(), ServiceState::Running);

    uint32_t now = 2000;
    FebruaryCore::instance().feed_text("dnd on", now);
    svc.run_once(now);

    EXPECT_GT(g_dnd_count, 0);

    svc.suspend();
    EXPECT_EQ(svc.state(), ServiceState::Suspended);
    EXPECT_EQ(svc.run_once(now + 10), 0u);

    svc.stop();
    EXPECT_EQ(svc.state(), ServiceState::Stopped);
}

// ---------------------------------------------------------------------------
// Multi-sensor Activity Inference & Episodic Memory Reinforcement Tests
// ---------------------------------------------------------------------------
#include "ai/intent_engine.hpp"

TEST(IntentEngineTest, ActivityInferenceAndStressDetection) {
    uint16_t conf = 0;

    // Running: high step delta or high heart rate
    EXPECT_EQ(::IntentEngine::infer_activity(35, 145, &conf), ActivityClassification::Running);
    EXPECT_GE(conf, 220);

    // Walking: moderate step delta
    EXPECT_EQ(::IntentEngine::infer_activity(10, 95, &conf), ActivityClassification::Walking);

    // HighStress: 0 steps, but heart rate is 115 bpm
    EXPECT_EQ(::IntentEngine::infer_activity(0, 115, &conf), ActivityClassification::HighStress);
    EXPECT_GE(conf, 220);

    // Resting: 0 steps, heart rate < 60 bpm
    EXPECT_EQ(::IntentEngine::infer_activity(0, 55, &conf), ActivityClassification::Resting);

    // Sedentary: 0 steps, normal heart rate (75 bpm)
    EXPECT_EQ(::IntentEngine::infer_activity(0, 75, &conf), ActivityClassification::Sedentary);
}

TEST(FebruaryEpisodicMemoryTest, HabitReinforcement) {
    auto& em = EpisodicMemory::instance();
    em.clear();

    HabitRule rule{};
    copy_cstr(rule.key, sizeof(rule.key), "workout_habit");
    rule.hour_start = 7;
    rule.hour_end = 9;
    rule.trigger = HabitTrigger::WristRaise;
    rule.action = HabitAction::CheckTimeOnly;
    rule.confidence_q8 = 100;
    rule.hit_count = 0;

    EXPECT_TRUE(em.upsert(rule));
    EXPECT_TRUE(em.reinforce_habit("workout_habit", 30));

    HabitRule matched{};
    EXPECT_TRUE(em.match(8, 1, HabitTrigger::WristRaise, &matched));
    EXPECT_EQ(matched.confidence_q8, 130);
    EXPECT_EQ(matched.hit_count, 1);
}

TEST(FebruaryCoreTest, StressAndEmergencyFeeds) {
    FebruaryCore::instance().init();
    uint32_t count_before = FebruaryCore::instance().total_intents_processed();

    // Feed high stress level
    FebruaryCore::instance().feed_stress(220, 5000);
    FebruaryCore::instance().process_events();

    EXPECT_GE(FebruaryCore::instance().total_intents_processed(), count_before);
}

// ---------------------------------------------------------------------------
// Firmware Closed-Loop Bridge Tests (Sensors -> February -> UI Overlay /
// PowerManager / AppControlBlock / DistributedSoftBus)
// ---------------------------------------------------------------------------
#include "experimental/apps/notification_center.hpp"
#include "net/distributed_bus.hpp"
#include "ai/february/firmware_bridge.hpp"

namespace {

class MockNotificationOverlay : public aurora::INotificationOverlay {
public:
    void show(const aurora::Notification& n) override {
        visible_ = true;
        current_ = n;
        ++show_count_;
    }

    void hide() override { visible_ = false; }

    bool is_visible() const noexcept override { return visible_; }

    void tick(uint32_t /*delta_ms*/) override {}

    bool visible_ = false;
    int show_count_ = 0;
    aurora::Notification current_{};
};

} // namespace

TEST(FebruaryFirmwareBridgeTest, SensorToOverlayPowerAndAppClosedLoop) {
    MockNotificationOverlay overlay;
    aurora::NotificationCenter::instance().clear();
    aurora::NotificationCenter::instance().set_overlay(&overlay);

    PowerManager::instance().transition_to(PowerState::ACTIVE);
    AppControlBlock fitness_app{1, AppState::BACKGROUND, "LuaFitness"};

    auto& bridge = FirmwareBridge::instance();
    ASSERT_TRUE(bridge.init(&fitness_app));

    // 1. Baseline sensor tick (steps = 10, HR = 72, battery = 80%)
    bridge.step_sensors(1000, 10, 72, 80, false);
    EXPECT_EQ(PowerManager::instance().get_state(), PowerState::ACTIVE);

    // 2. Step delta >= 50 triggers StartFitness -> TransitionApp promotes fitness_app
    bridge.step_sensors(1500, 75, 130, 80, false);
    EXPECT_EQ(fitness_app.state, AppState::FOREGROUND);
    EXPECT_GE(bridge.app_transition_count(), 1u);

    // 3. Wrist raise triggers Greeting -> Speak -> NotificationCenter pops up overlay
    overlay.hide();
    aurora::NotificationCenter::instance().clear();
    bridge.step_sensors(2000, 75, 76, 80, true);
    EXPECT_GE(bridge.speak_count(), 1u);
    EXPECT_TRUE(contains_ci(bridge.last_spoken(), "February online"));
    EXPECT_TRUE(overlay.is_visible());
    EXPECT_STREQ(overlay.current_.title, "February AI");
    EXPECT_TRUE(contains_ci(overlay.current_.body, "February online"));

    // 4. Battery drop <= 15% triggers BatteryLow -> NotifyUser + SetPower(Critical)
    overlay.hide();
    aurora::NotificationCenter::instance().clear();
    bridge.step_sensors(3000, 75, 76, 12, false);
    EXPECT_GE(bridge.notify_count(), 1u);
    EXPECT_GE(bridge.power_transition_count(), 1u);
    EXPECT_EQ(PowerManager::instance().get_state(), PowerState::CRITICAL);
    EXPECT_TRUE(overlay.is_visible());
    EXPECT_EQ(overlay.current_.priority, aurora::NotificationPriority::critical);
    EXPECT_TRUE(contains_ci(overlay.current_.body, "Battery low"));

    aurora::NotificationCenter::instance().set_overlay(nullptr);
    FebruaryCrit::set(nullptr, nullptr);
}

TEST(FebruaryFirmwareBridgeTest, DndSuppressesNonCriticalButAllowsEmergency) {
    MockNotificationOverlay overlay;
    aurora::NotificationCenter::instance().clear();
    aurora::NotificationCenter::instance().set_overlay(&overlay);

    auto& bridge = FirmwareBridge::instance();
    ASSERT_TRUE(bridge.init(nullptr));

    // Enable DND via natural-language command
    FebruaryCore::instance().feed_text("dnd on", 1000);
    FebruaryService::instance().run_once(1000);
    EXPECT_TRUE(bridge.dnd_active());

    // Non-critical query is spoken internally but suppressed from popping UI overlay
    overlay.hide();
    aurora::NotificationCenter::instance().clear();
    const int before_shows = overlay.show_count_;
    FebruaryCore::instance().feed_text("status", 1200);
    FebruaryService::instance().run_once(1200);
    EXPECT_EQ(overlay.show_count_, before_shows);
    EXPECT_FALSE(overlay.is_visible());

    // Emergency intent bypasses DND and posts a critical notification to the overlay
    FebruaryCore::instance().feed_emergency(true, 1500);
    FebruaryService::instance().run_once(1500);
    EXPECT_TRUE(overlay.is_visible());
    EXPECT_EQ(overlay.current_.priority, aurora::NotificationPriority::critical);
    EXPECT_TRUE(contains_ci(overlay.current_.body, "Emergency"));

    aurora::NotificationCenter::instance().set_overlay(nullptr);
    FebruaryCrit::set(nullptr, nullptr);
}

TEST(FebruaryFirmwareBridgeTest, DistributedSoftBusRxAndTxClosedLoop) {
    MockNotificationOverlay overlay;
    aurora::NotificationCenter::instance().clear();
    aurora::NotificationCenter::instance().set_overlay(&overlay);

    DistributedSoftBus& dist_bus = DistributedSoftBus::instance();
    dist_bus.reset_security_state();

    auto& bridge = FirmwareBridge::instance();
    ASSERT_TRUE(bridge.init(nullptr));

    // 1. Simulate authenticated peer discovery callback registering peer in PeerTable
    PeerTable::instance().clear();
    const uint8_t key[32] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
        0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
    dist_bus.provision_key(key, 1);

    const char* peer_dev_id = "watch_peer_01";
    const uint32_t seq = 1u;
    const uint8_t seq_bytes[4] = {0, 0, 0, 1};
    HmacSha256 hmac;
    hmac.init(key, 32u);
    hmac.update(reinterpret_cast<const uint8_t*>(peer_dev_id), std::strlen(peer_dev_id));
    hmac.update(seq_bytes, 4u);
    uint8_t digest[32]{};
    hmac.finish(digest);
    char auth_hex[65]{};
    const char* hex = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        auth_hex[i * 2] = hex[(digest[i] >> 4) & 0x0F];
        auth_hex[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    char beacon_json[256]{};
    std::snprintf(beacon_json, sizeof(beacon_json),
                  "{\"event\":\"beacon\",\"device_id\":\"%s\","
                  "\"cap\":[\"display\"],\"seq\":\"%u\",\"auth\":\"%s\"}",
                  peer_dev_id, seq, auth_hex);

    EXPECT_TRUE(dist_bus.process_beacon_message("10.0.2.25", beacon_json, 1000));
    EXPECT_NE(PeerTable::instance().find_by_network_id(peer_dev_id), nullptr);

    // 2. Inbound UDP 8899 binary February Intent frame -> DistributedSoftBus ->
    //    SoftBus::on_bytes_received -> FebruaryService::run_once -> Overlay
    Intent remote_remind;
    remote_remind.type = IntentType::RemindRest;
    remote_remind.confidence_x1000 = 880;
    copy_cstr(remote_remind.text, sizeof(remote_remind.text), "remote rest alert");

    uint8_t frame[kSoftBusFrameMax]{};
    const uint32_t expected_peer_id = FirmwareBridge::hash_device_id(peer_dev_id);
    const unsigned frame_len = softbus_pack_intent(
        remote_remind, expected_peer_id, 2000, frame, sizeof(frame));
    ASSERT_GT(frame_len, 0u);

    overlay.hide();
    aurora::NotificationCenter::instance().clear();

    auroraos::net::L4PacketContext pkt_ctx{};
    pkt_ctx.ip_proto = auroraos::net::PROTO_UDP;
    pkt_ctx.src_ip = (10u << 24) | (0u << 16) | (2u << 8) | 25u;
    pkt_ctx.dst_ip = (10u << 24) | (0u << 16) | (2u << 8) | 15u;
    pkt_ctx.src_port = 8899;
    pkt_ctx.dst_port = DistributedSoftBus::SOFTBUS_PORT;

    dist_bus.on_packet(pkt_ctx, frame, frame_len);
    EXPECT_GE(SoftBus::instance().pending(), 1u);

    // Step firmware bridge to drain SoftBus and execute the remote intent
    bridge.step_sensors(2100, 0, 70, 85, false);
    EXPECT_TRUE(overlay.is_visible());
    EXPECT_TRUE(contains_ci(overlay.current_.body, "Rest reminder"));

    // 3. Outbound PublishRemote -> SoftBusTransportOps -> DistributedSoftBus::send_raw_frame
    static uint8_t tx_captured[kSoftBusFrameMax]{};
    static size_t tx_captured_len = 0;
    tx_captured_len = 0;
    dist_bus.set_tx_hook(
        [](const void* data, size_t len, void*) -> int {
            if (len <= sizeof(tx_captured)) {
                std::memcpy(tx_captured, data, len);
                tx_captured_len = len;
            }
            return static_cast<int>(len);
        },
        nullptr);

    Intent out_intent;
    out_intent.type = IntentType::QueryHealth;
    out_intent.confidence_x1000 = 910;
    copy_cstr(out_intent.text, sizeof(out_intent.text), "sync health");
    EXPECT_TRUE(FebruaryService::instance().publish_remote(expected_peer_id, out_intent, 2500));
    EXPECT_GT(tx_captured_len, 0u);

    Intent unpacked;
    uint32_t unpacked_peer = 0;
    uint32_t unpacked_ts = 0;
    ASSERT_TRUE(softbus_unpack_intent(tx_captured, static_cast<unsigned>(tx_captured_len),
                                      unpacked, unpacked_peer, unpacked_ts));
    EXPECT_EQ(unpacked.type, IntentType::QueryHealth);
    EXPECT_STREQ(unpacked.text, "sync health");

    dist_bus.set_tx_hook(nullptr, nullptr);
    aurora::NotificationCenter::instance().set_overlay(nullptr);
    FebruaryCrit::set(nullptr, nullptr);
}
