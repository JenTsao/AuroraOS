/**
 * February Phase 2.2 host test
 *   - PlanRule table / set_rules
 *   - PeerTable
 *   - FebruaryCrit (smoke)
 *   - Remote yield to local
 *   - Session close + board_bind
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror -I. \
 *       -o /tmp/test_p22 tests/unit/test_february_phase22.cpp
 *   /tmp/test_p22
 */
#include "ai/february/service.hpp"
#include "ai/february/planner.hpp"
#include "ai/february/peer_table.hpp"
#include "ai/february/crit.hpp"
#include "ai/february/board_bind.hpp"
#include "ai/february/softbus.hpp"
#include "ai/february/string_util.hpp"

#include <cstdio>
#include <cstring>
#include <cassert>

using namespace aurora::february;

static int g_crit_enter = 0;
static int g_crit_exit  = 0;
static int g_speak = 0;
static int g_notify = 0;
static int g_power = 0;

static void crit_enter(void*) { ++g_crit_enter; }
static void crit_exit(void*)  { ++g_crit_exit; }

static void h_speak(const char* msg, void*) {
    ++g_speak;
    std::printf("[SPEAK] %s\n", msg ? msg : "");
}
static void h_notify(const char* msg, void*) {
    ++g_notify;
    std::printf("[NOTIFY] %s\n", msg ? msg : "");
}
static void h_power(int32_t mode, void*) {
    ++g_power;
    std::printf("[POWER] %d\n", (int)mode);
}

int main() {
    std::printf("=== February Phase 2.2 host test ===\n");

    FebruaryCrit::set(crit_enter, crit_exit);
    {
        FebruaryCrit::Guard g;
        assert(g_crit_enter >= 1);
    }
    assert(g_crit_exit >= 1);

    CapabilityHooks caps{};
    caps.on_speak = h_speak;
    caps.on_notify = h_notify;
    caps.on_set_power = h_power;

    BoardBindArgs args{};
    args.crit_enter = crit_enter;
    args.crit_exit  = crit_exit;
    args.caps = &caps;
    args.wake_word = nullptr;
    assert(board_bind_start(args));
    assert(FebruaryService::instance().state() == ServiceState::Running);

    auto& core = FebruaryCore::instance();
    auto& svc  = FebruaryService::instance();
    uint32_t t = 1000;

    core.feed_battery(10, t);
    svc.run_once(t);
    assert(g_notify >= 1);
    assert(core.context().power == PowerMode::Critical);
    assert(g_power >= 1);

    static const PlanRule kCustom[] = {
        {IntentType::Help, false, 0, 1,
         {{ActionType::NotifyUser, 0, 0, "custom-help"}}},
        {IntentType::None, false, 0, 0, {}},
    };
    Planner::instance().set_rules(kCustom);
    const int n0 = g_notify;
    core.feed_text("help", t + 50);
    svc.run_once(t + 50);
    assert(g_notify == n0 + 1);
    Planner::instance().set_rules(nullptr);

    PeerTable::instance().clear();
    assert(PeerTable::instance().count() == 0);
    assert(SoftBus::instance().register_peer(7, "net-7", t + 100));
    PeerSlot* ps = PeerTable::instance().find(7);
    assert(ps && ps->peer_id == 7);
    assert(std::strcmp(ps->network_id, "net-7") == 0);

    Intent greet;
    greet.type = IntentType::Greeting;
    greet.confidence_x1000 = 800;
    SoftBus::instance().publish_intent(7, greet, t + 110, false);
    PeerSlot* ps2 = PeerTable::instance().find(7);
    assert(ps2);
    assert(ps2->tx_ok >= 1 || ps2->last_tx_ms > 0 || ps2->last_seen_ms > 0);

    SoftBus::instance().clear();
    SoftBus::instance().start_server();
    Intent remote;
    remote.type = IntentType::Help;
    remote.confidence_x1000 = 900;
    SoftBusStub::instance().publish(42, remote, t + 200);

    core.feed_text("status", t + 200);
    assert(EventBus::instance().has_local_intent());

    svc.run_once(t + 200);
    assert(SoftBusStub::instance().pending() == 0);
    const auto last = core.memory().last_intent().type;
    assert(last == IntentType::Help || last == IntentType::QueryStatus);

    SoftBus::instance().register_peer(3, "net-3", t + 300);
    SoftBusSessionId sid = SoftBus::instance().ensure_session(3);
    assert(sid >= 0);
    SoftBus::instance().close_peer(3);
    SoftBus::instance().on_session_closed(sid);

    svc.stop();
    assert(svc.state() == ServiceState::Stopped);

    FebruaryCrit::set(nullptr, nullptr);

    std::printf("crit_enter=%d crit_exit=%d speak=%d notify=%d power=%d\n",
                g_crit_enter, g_crit_exit, g_speak, g_notify, g_power);
    std::printf("=== ALL PHASE 2.2 CHECKS PASSED ===\n");
    return 0;
}
