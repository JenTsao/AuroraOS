/**
 * February Phase 2.2 host test (GoogleTest)
 *   - PlanRule table / set_rules
 *   - PeerTable
 *   - FebruaryCrit (smoke)
 *   - Remote yield to local
 *   - Session close + board_bind
 *
 * Originally a standalone program with its own main() and hand-written
 * asserts. Kept as a single ordered flow because the scenario shares
 * process-global service/bus state at each step.
 */
#include <gtest/gtest.h>

#include <cstring>

#include "ai/february/service.hpp"
#include "ai/february/planner.hpp"
#include "ai/february/peer_table.hpp"
#include "ai/february/crit.hpp"
#include "ai/february/board_bind.hpp"
#include "ai/february/softbus.hpp"
#include "ai/february/string_util.hpp"

using namespace aurora::february;

namespace {

int g_crit_enter = 0;
int g_crit_exit = 0;
int g_speak = 0;
int g_notify = 0;
int g_power = 0;

void crit_enter(void*) { ++g_crit_enter; }
void crit_exit(void*) { ++g_crit_exit; }

void h_speak(const char*, void*) { ++g_speak; }
void h_notify(const char*, void*) { ++g_notify; }
void h_power(int32_t, void*) { ++g_power; }

}  // namespace

class FebruaryPhase22Test : public ::testing::Test {
protected:
    void SetUp() override {
        g_crit_enter = 0;
        g_crit_exit = 0;
        g_speak = 0;
        g_notify = 0;
        g_power = 0;
        FebruaryCore::instance().reset();
        EventBus::instance().clear();
    }

    void TearDown() override {
        FebruaryCrit::set(nullptr, nullptr);
    }
};

TEST_F(FebruaryPhase22Test, CritPlannerPeerTableAndRemoteYield) {
    FebruaryCrit::set(crit_enter, crit_exit);
    {
        FebruaryCrit::Guard g;
        EXPECT_GE(g_crit_enter, 1);
    }
    EXPECT_GE(g_crit_exit, 1);

    CapabilityHooks caps{};
    caps.on_speak = h_speak;
    caps.on_notify = h_notify;
    caps.on_set_power = h_power;

    BoardBindArgs args{};
    args.crit_enter = crit_enter;
    args.crit_exit = crit_exit;
    args.caps = &caps;
    args.wake_word = nullptr;
    ASSERT_TRUE(board_bind_start(args));
    EXPECT_EQ(FebruaryService::instance().state(), ServiceState::Running);

    auto& core = FebruaryCore::instance();
    auto& svc = FebruaryService::instance();
    uint32_t t = 1000;

    core.feed_battery(10, t);
    svc.run_once(t);
    EXPECT_GE(g_notify, 1);
    EXPECT_EQ(core.context().power, PowerMode::Critical);
    EXPECT_GE(g_power, 1);

    static const PlanRule kCustom[] = {
        {IntentType::Help, false, 0, 1,
         {{ActionType::NotifyUser, 0, 0, "custom-help"}}},
        {IntentType::None, false, 0, 0, {}},
    };
    Planner::instance().set_rules(kCustom);
    const int n0 = g_notify;
    core.feed_text("help", t + 50);
    svc.run_once(t + 50);
    EXPECT_EQ(g_notify, n0 + 1);
    Planner::instance().set_rules(nullptr);

    PeerTable::instance().clear();
    EXPECT_EQ(PeerTable::instance().count(), 0u);
    EXPECT_TRUE(SoftBus::instance().register_peer(7, "net-7", t + 100));
    PeerSlot* ps = PeerTable::instance().find(7);
    ASSERT_NE(ps, nullptr);
    EXPECT_EQ(ps->peer_id, 7u);
    EXPECT_STREQ(ps->network_id, "net-7");

    Intent greet;
    greet.type = IntentType::Greeting;
    greet.confidence_x1000 = 800;
    SoftBus::instance().publish_intent(7, greet, t + 110, false);
    PeerSlot* ps2 = PeerTable::instance().find(7);
    ASSERT_NE(ps2, nullptr);
    EXPECT_TRUE(ps2->tx_ok >= 1 || ps2->last_tx_ms > 0 || ps2->last_seen_ms > 0);

    SoftBus::instance().clear();
    SoftBus::instance().start_server();
    Intent remote;
    remote.type = IntentType::Help;
    remote.confidence_x1000 = 900;
    SoftBusStub::instance().publish(42, remote, t + 200);

    core.feed_text("status", t + 200);
    EXPECT_TRUE(EventBus::instance().has_local_intent());

    svc.run_once(t + 200);
    EXPECT_EQ(SoftBusStub::instance().pending(), 0u);
    const auto last = core.memory().last_intent().type;
    EXPECT_TRUE(last == IntentType::Help || last == IntentType::QueryStatus);

    SoftBus::instance().register_peer(3, "net-3", t + 300);
    SoftBusSessionId sid = SoftBus::instance().ensure_session(3);
    EXPECT_GE(sid, 0);
    SoftBus::instance().close_peer(3);
    SoftBus::instance().on_session_closed(sid);

    svc.stop();
    EXPECT_EQ(svc.state(), ServiceState::Stopped);
}
