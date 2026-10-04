/**
 * February Phase 2 host test (GoogleTest).
 */
#include <gtest/gtest.h>

#include <cstring>

#include "ai/february/service.hpp"
#include "ai/february/planner.hpp"
#include "ai/february/softbus_stub.hpp"
#include "ai/february/platform_hooks.hpp"
#include "ai/february/string_util.hpp"

using namespace aurora::february;

namespace {

int g_speak = 0;
int g_dnd = 0;
int g_app = 0;
int g_notify = 0;
int g_remote = 0;
char g_last_speak[128];

void h_speak(const char* msg, void*) {
    ++g_speak;
    std::snprintf(g_last_speak, sizeof(g_last_speak), "%s", msg ? msg : "");
}

void h_dnd(bool, void*) { ++g_dnd; }
void h_app(int32_t, int32_t, void*) { ++g_app; }
void h_notify(const char*, void*) { ++g_notify; }
void h_remote(uint32_t, const Intent*, void*) { ++g_remote; }

}  // namespace

class FebruaryPhase2Test : public ::testing::Test {
protected:
    void SetUp() override {
        g_speak = 0;
        g_dnd = 0;
        g_app = 0;
        g_notify = 0;
        g_remote = 0;
        g_last_speak[0] = '\0';
        FebruaryService::instance().stop();
        FebruaryCore::instance().reset();
        EventBus::instance().clear();
        SoftBusStub::instance().clear();
        SoftBus::instance().clear();
        SoftBus::instance().bind_transport(SoftBusTransportOps{});
    }
};

TEST_F(FebruaryPhase2Test, ServicePlannerSoftBusAndHooks) {
    EXPECT_TRUE(contains_ci("Hey February Status", "february"));
    EXPECT_FALSE(contains_ci("hello", "xyz"));
    char buf[8];
    EXPECT_EQ(copy_cstr(buf, sizeof(buf), "abcdefghi"), 7u);
    EXPECT_STREQ(buf, "abcdefg");

    FebruaryService& svc = FebruaryService::instance();
    EXPECT_EQ(svc.state(), ServiceState::Stopped);
    EXPECT_TRUE(svc.start());
    EXPECT_EQ(svc.state(), ServiceState::Running);

    CapabilityHooks caps;
    caps.on_speak = h_speak;
    caps.on_set_dnd = h_dnd;
    caps.on_transition_app = h_app;
    caps.on_notify = h_notify;
    caps.on_publish_remote = h_remote;
    svc.set_capability_hooks(caps);

    uint32_t t = 1000;
    auto& core = FebruaryCore::instance();

    core.feed_text("do not disturb", t);
    svc.run_once(t);
    EXPECT_TRUE(core.context().dnd);
    EXPECT_GE(g_dnd, 1);
    EXPECT_GE(g_speak, 1);

    const int apps0 = g_app;
    core.feed_steps(0, t + 10);
    core.feed_steps(80, t + 20);
    svc.run_once(t + 20);
    EXPECT_EQ(g_app, apps0 + 1);

    const int n0 = g_notify;
    core.feed_battery(10, t + 100);
    svc.run_once(t + 100);
    EXPECT_GE(g_notify, n0 + 1);
    EXPECT_EQ(core.context().power, PowerMode::Critical);
    EXPECT_GE(g_speak, 2);

    Intent remote;
    remote.type = IntentType::Help;
    remote.confidence_x1000 = 900;
    SoftBusStub::instance().publish(42, remote, t + 200);
    EXPECT_EQ(SoftBusStub::instance().pending(), 1u);

    const int speak_before = g_speak;
    svc.run_once(t + 200);
    EXPECT_EQ(SoftBusStub::instance().pending(), 0u);
    EXPECT_GT(g_speak, speak_before);
    EXPECT_EQ(core.memory().last_intent().type, IntentType::Help);

    Intent out;
    out.type = IntentType::Greeting;
    out.confidence_x1000 = 800;
    const int r0 = g_remote;
    const bool delivered = svc.publish_remote(7, out, t + 300);
    EXPECT_EQ(g_remote, r0 + 1);
    EXPECT_FALSE(delivered);
    EXPECT_EQ(SoftBusStub::instance().pending(), 0u);

    EXPECT_TRUE(svc.publish_remote(0, out, t + 310));
    EXPECT_GE(SoftBusStub::instance().pending(), 1u);
    svc.run_once(t + 310);

    svc.suspend();
    EXPECT_EQ(svc.state(), ServiceState::Suspended);
    const int s0 = g_speak;
    core.feed_text("status", t + 400);
    EXPECT_EQ(svc.run_once(t + 400), 0u);
    svc.resume();
    svc.run_once(t + 400);
    EXPECT_EQ(svc.state(), ServiceState::Running);
    EXPECT_GE(g_speak, s0);

    core.feed_text("status", t + 500);
    svc.run_once(t + 500);
    EXPECT_TRUE(std::strstr(g_last_speak, "Steps") != nullptr ||
                std::strstr(g_last_speak, "steps") != nullptr ||
                g_speak > s0);

    svc.stop();
    EXPECT_EQ(svc.state(), ServiceState::Stopped);

    core.feed_text("help", t + 600);
    core.process_events();
    EXPECT_EQ(core.memory().last_intent().type, IntentType::Help);
}
