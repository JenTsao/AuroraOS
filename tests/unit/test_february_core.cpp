/**
 * February Phase 1.5 end-to-end host test (GoogleTest).
 */
#include <gtest/gtest.h>

#include <cstring>

#include "ai/february/february_core.hpp"

using namespace aurora::february;

namespace {

int g_speak_count = 0;
int g_dnd_calls = 0;
int g_last_dnd = -1;
int g_app_calls = 0;
char g_last_speak[128];

void hook_speak(const char* msg, void*) {
    ++g_speak_count;
    std::snprintf(g_last_speak, sizeof(g_last_speak), "%s", msg ? msg : "");
}

void hook_dnd(bool enable, void*) {
    ++g_dnd_calls;
    g_last_dnd = enable ? 1 : 0;
}

void hook_app(int32_t app_id, int32_t state, void*) {
    ++g_app_calls;
    (void)app_id;
    (void)state;
}

}  // namespace

class FebruaryCorePhase1Test : public ::testing::Test {
protected:
    void SetUp() override {
        g_speak_count = 0;
        g_dnd_calls = 0;
        g_last_dnd = -1;
        g_app_calls = 0;
        g_last_speak[0] = '\0';
    }
};

TEST_F(FebruaryCorePhase1Test, EndToEndPhase1Flow) {
    FebruaryCore& f = FebruaryCore::instance();
    f.init();
    EXPECT_TRUE(f.ready());

    ActionHooks hooks;
    hooks.on_speak = hook_speak;
    hooks.on_set_dnd = hook_dnd;
    hooks.on_transition_app = hook_app;
    f.set_action_hooks(hooks);

    uint32_t t = 1000;

    f.feed_text("hello February", t);
    f.process_events();
    EXPECT_GE(g_speak_count, 1);
    EXPECT_EQ(f.memory().last_intent().type, IntentType::Greeting);

    f.feed_steps(30, t);
    f.process_events();
    f.feed_steps(40, t + 5);
    f.process_events();
    f.feed_heart_rate(72, t);
    f.feed_battery(85, t);
    f.feed_text("STATUS", t + 10);
    f.process_events();
    EXPECT_NE(std::strstr(g_last_speak, "Steps"), nullptr);

    f.feed_text("do not disturb", t + 20);
    f.process_events();
    EXPECT_TRUE(f.context().dnd);
    EXPECT_EQ(g_last_dnd, 1);
    EXPECT_TRUE(f.memory().dnd_on());

    f.feed_text("cancel dnd", t + 25);
    f.process_events();
    EXPECT_FALSE(f.context().dnd);
    EXPECT_EQ(g_last_dnd, 0);
    EXPECT_FALSE(f.memory().dnd_on());

    f.feed_text("Help", t + 30);
    f.process_events();
    f.feed_text("xyzzy", t + 40);
    f.process_events();
    EXPECT_EQ(f.memory().last_intent().type, IntentType::UnknownCommand);

    const int apps_before = g_app_calls;
    f.feed_steps(100, t + 100);
    f.process_events();
    EXPECT_EQ(g_app_calls, apps_before + 1);
    f.feed_steps(170, t + 200);
    f.process_events();
    EXPECT_EQ(g_app_calls, apps_before + 1);
    f.feed_steps(240, t + 100 + 60000);
    f.process_events();
    EXPECT_EQ(g_app_calls, apps_before + 2);

    t = t + 100 + 60000 + 1000;
    for (int i = 0; i < 50; ++i) {
        t += 1000;
        f.tick(t);
        f.feed_steps(240, t);
        f.process_events();
    }
    EXPECT_GE(f.context().idle_seconds, 40u);

    const int speaks_before = g_speak_count;
    f.feed_battery(10, t + 1000);
    f.process_events();
    f.feed_battery(10, t + 2000);
    f.process_events();
    EXPECT_EQ(g_speak_count, speaks_before + 1);
    f.feed_battery(50, t + 3000);
    f.process_events();
    f.feed_battery(10, t + 4000);
    f.process_events();
    EXPECT_EQ(g_speak_count, speaks_before + 2);

    f.set_wake_word("hey february");
    const int intents_before = static_cast<int>(f.memory().intent_count());
    const int speaks_w = g_speak_count;
    f.feed_text("status", t + 5000);
    f.process_events();
    EXPECT_EQ(static_cast<int>(f.memory().intent_count()), intents_before);
    EXPECT_EQ(g_speak_count, speaks_w);
    f.feed_text("hey february status", t + 5100);
    f.process_events();
    EXPECT_EQ(f.memory().last_intent().type, IntentType::QueryStatus);

    f.set_wake_word("");
    f.feed_text("help", t + 5200);
    f.process_events();
    EXPECT_EQ(f.memory().last_intent().type, IntentType::Help);
    EXPECT_GT(f.memory().speak_count(), 0u);
}
