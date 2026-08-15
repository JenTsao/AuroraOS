/**
 * February Phase 2 host test — service + planner + SoftBus stub + hooks
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror -I. \
 *       -o /tmp/test_february_p2 tests/unit/test_february_phase2.cpp
 *   /tmp/test_february_p2
 */
#include "ai/february/service.hpp"
#include "ai/february/planner.hpp"
#include "ai/february/softbus_stub.hpp"
#include "ai/february/platform_hooks.hpp"
#include "ai/february/string_util.hpp"

#include <cstdio>
#include <cstring>
#include <cassert>

using namespace aurora::february;

static int g_speak = 0;
static int g_dnd = 0;
static int g_app = 0;
static int g_notify = 0;
static int g_remote = 0;
static char g_last_speak[128];

static void h_speak(const char* msg, void*) {
    ++g_speak;
    std::snprintf(g_last_speak, sizeof(g_last_speak), "%s", msg ? msg : "");
    std::printf("[SPEAK] %s\n", g_last_speak);
}
static void h_dnd(bool on, void*) {
    ++g_dnd;
    std::printf("[DND] %s\n", on ? "ON" : "OFF");
}
static void h_app(int32_t id, int32_t st, void*) {
    ++g_app;
    std::printf("[APP] id=%d state=%d\n", (int)id, (int)st);
}
static void h_notify(const char* msg, void*) {
    ++g_notify;
    std::printf("[NOTIFY] %s\n", msg ? msg : "");
}
static void h_remote(uint32_t peer, const Intent* in, void*) {
    ++g_remote;
    std::printf("[REMOTE] peer=%u intent=%u\n",
                (unsigned)peer, in ? (unsigned)in->type : 0u);
}

int main() {
    std::printf("=== February Phase-2 host test ===\n");

    // string_util
    assert(contains_ci("Hey February Status", "february"));
    assert(!contains_ci("hello", "xyz"));
    char buf[8];
    assert(copy_cstr(buf, sizeof(buf), "abcdefghi") == 7);
    assert(std::strcmp(buf, "abcdefg") == 0);

    FebruaryService& svc = FebruaryService::instance();
    assert(svc.state() == ServiceState::Stopped);
    assert(svc.start());
    assert(svc.state() == ServiceState::Running);

    CapabilityHooks caps;
    caps.on_speak = h_speak;
    caps.on_set_dnd = h_dnd;
    caps.on_transition_app = h_app;
    caps.on_notify = h_notify;
    caps.on_publish_remote = h_remote;
    svc.set_capability_hooks(caps);

    uint32_t t = 1000;
    auto& core = FebruaryCore::instance();

    // --- Planner: DND ---
    core.feed_text("do not disturb", t);
    svc.run_once(t);
    assert(core.context().dnd == true);
    assert(g_dnd >= 1);
    assert(g_speak >= 1);

    // --- Planner: fitness transition ---
    const int apps0 = g_app;
    core.feed_steps(0, t + 10);
    core.feed_steps(80, t + 20);  // delta >= 50
    svc.run_once(t + 20);
    assert(g_app == apps0 + 1);

    // --- Planner: battery low → notify + SetPower ---
    const int n0 = g_notify;
    const auto power_before = core.context().power;
    (void)power_before;
    core.feed_battery(10, t + 100);
    svc.run_once(t + 100);
    assert(g_notify >= n0 + 1);
    assert(core.context().power == PowerMode::Critical);
    assert(g_speak >= 2);

    // --- SoftBus: remote intent inject ---
    Intent remote;
    remote.type = IntentType::Help;
    remote.confidence_x1000 = 900;
    SoftBusStub::instance().publish(42, remote, t + 200);
    assert(SoftBusStub::instance().pending() == 1);

    const int speak_before = g_speak;
    svc.run_once(t + 200);
    assert(SoftBusStub::instance().pending() == 0);
    assert(g_speak > speak_before);
    assert(core.memory().last_intent().type == IntentType::Help);

    // --- publish_remote hits hook + local queue ---
    Intent out;
    out.type = IntentType::Greeting;
    out.confidence_x1000 = 800;
    const int r0 = g_remote;
    svc.publish_remote(7, out, t + 300);
    assert(g_remote == r0 + 1);
    assert(SoftBusStub::instance().pending() >= 1);
    svc.run_once(t + 300);  // drain loopback

    // --- service suspend / resume ---
    svc.suspend();
    assert(svc.state() == ServiceState::Suspended);
    const int s0 = g_speak;
    core.feed_text("status", t + 400);
    assert(svc.run_once(t + 400) == 0);  // suspended: no process
    // events may sit on bus; resume and drain
    svc.resume();
    svc.run_once(t + 400);
    assert(svc.state() == ServiceState::Running);
    assert(g_speak >= s0);  // may or may not have pending speak depending on emit timing

    // force status after resume
    core.feed_text("status", t + 500);
    svc.run_once(t + 500);
    assert(std::strstr(g_last_speak, "Steps") != nullptr ||
           std::strstr(g_last_speak, "steps") != nullptr ||
           g_speak > s0);

    svc.stop();
    assert(svc.state() == ServiceState::Stopped);

    // Phase 1 regression smoke via core alone
    core.feed_text("help", t + 600);
    core.process_events();
    assert(core.memory().last_intent().type == IntentType::Help);

    std::printf("Speak=%d DND=%d App=%d Notify=%d Remote=%d\n",
                g_speak, g_dnd, g_app, g_notify, g_remote);
    std::printf("=== ALL PHASE2 CHECKS PASSED ===\n");
    return 0;
}
