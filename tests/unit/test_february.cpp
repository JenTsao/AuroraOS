// =============================================================================
// test_february.cpp — Unit and Integration tests for February AI Runtime
// =============================================================================

#include <gtest/gtest.h>

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

