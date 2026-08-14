/**
 * February Phase 1.5 host test
 * g++ -std=c++17 -Wall -Wextra -Werror -I. -o /tmp/test_february tests/unit/test_february_core.cpp
 */
#include "ai/february/february_core.hpp"
#include <cstdio>
#include <cstring>
#include <cassert>

using namespace aurora::february;

static int g_speak_count = 0, g_dnd_calls = 0, g_last_dnd = -1, g_app_calls = 0;
static char g_last_speak[128];

static void hook_speak(const char* msg, void*) {
    ++g_speak_count;
    std::snprintf(g_last_speak, sizeof(g_last_speak), "%s", msg ? msg : "");
    std::printf("[SPEAK] %s\n", g_last_speak);
}
static void hook_dnd(bool enable, void*) {
    ++g_dnd_calls; g_last_dnd = enable ? 1 : 0;
    std::printf("[DND] %s\n", enable ? "ON" : "OFF");
}
static void hook_app(int32_t app_id, int32_t state, void*) {
    ++g_app_calls;
    std::printf("[APP] id=%d state=%d\n", (int)app_id, (int)state);
}

int main() {
    std::printf("=== February Core Phase-1 host test ===\n");
    FebruaryCore& f = FebruaryCore::instance();
    f.init();
    assert(f.ready());

    ActionHooks hooks;
    hooks.on_speak = hook_speak;
    hooks.on_set_dnd = hook_dnd;
    hooks.on_transition_app = hook_app;
    f.set_action_hooks(hooks);

    uint32_t t = 1000;

    f.feed_text("hello February", t);
    f.process_events();
    assert(g_speak_count >= 1);
    assert(f.memory().last_intent().type == IntentType::Greeting);

    f.feed_steps(30, t);
    f.process_events();
    f.feed_steps(40, t + 5);
    f.process_events();
    f.feed_heart_rate(72, t);
    f.feed_battery(85, t);
    f.feed_text("STATUS", t + 10);
    f.process_events();
    assert(std::strstr(g_last_speak, "Steps") != nullptr);

    f.feed_text("do not disturb", t + 20);
    f.process_events();
    assert(f.context().dnd && g_last_dnd == 1 && f.memory().dnd_on());

    f.feed_text("cancel dnd", t + 25);
    f.process_events();
    assert(!f.context().dnd && g_last_dnd == 0 && !f.memory().dnd_on());

    f.feed_text("Help", t + 30);
    f.process_events();
    f.feed_text("xyzzy", t + 40);
    f.process_events();
    assert(f.memory().last_intent().type == IntentType::UnknownCommand);

    const int apps_before = g_app_calls;
    f.feed_steps(100, t + 100);
    f.process_events();
    assert(g_app_calls == apps_before + 1);
    f.feed_steps(170, t + 200);
    f.process_events();
    assert(g_app_calls == apps_before + 1);
    f.feed_steps(240, t + 100 + 60000);
    f.process_events();
    assert(g_app_calls == apps_before + 2);

    t = t + 100 + 60000 + 1000;
    for (int i = 0; i < 50; ++i) {
        t += 1000;
        f.tick(t);
        f.feed_steps(240, t);
        f.process_events();
    }
    assert(f.context().idle_seconds >= 40);

    const int speaks_before = g_speak_count;
    f.feed_battery(10, t + 1000);
    f.process_events();
    f.feed_battery(10, t + 2000);
    f.process_events();
    assert(g_speak_count == speaks_before + 1);
    f.feed_battery(50, t + 3000);
    f.process_events();
    f.feed_battery(10, t + 4000);
    f.process_events();
    assert(g_speak_count == speaks_before + 2);

    f.set_wake_word("hey february");
    const int intents_before = (int)f.memory().intent_count();
    const int speaks_w = g_speak_count;
    f.feed_text("status", t + 5000);
    f.process_events();
    assert((int)f.memory().intent_count() == intents_before);
    assert(g_speak_count == speaks_w);
    f.feed_text("hey february status", t + 5100);
    f.process_events();
    assert(f.memory().last_intent().type == IntentType::QueryStatus);

    f.set_wake_word("");
    f.feed_text("help", t + 5200);
    f.process_events();
    assert(f.memory().last_intent().type == IntentType::Help);
    assert(f.memory().speak_count() > 0);

    std::printf("=== ALL CHECKS PASSED ===\n");
    return 0;
}
