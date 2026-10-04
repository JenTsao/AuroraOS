/**
 * SensorAggregator host test (GoogleTest).
 */
#include <gtest/gtest.h>

#include "ai/february/february_core.hpp"
#include "ai/february/sensor_aggregator.hpp"

using namespace aurora::february;

namespace {

int g_fused = 0;
int g_time_ev = 0;
int g_prio_high = 0;
int g_prio_low = 0;

void on_fused(const Event& ev, void*) {
    if (ev.type == EventType::SensorFused) {
        ++g_fused;
        EXPECT_LE(ev.payload.sensor.confidence_q8, 255);
    }
}

void on_time(const Event& ev, void*) {
    if (ev.type == EventType::TimeContextChanged) ++g_time_ev;
}

void on_high(const Event&, void*) { ++g_prio_high; }

void on_low(const Event&, void*) {
    EXPECT_GE(g_prio_high, 1);
    ++g_prio_low;
}

}  // namespace

class FebruarySensorAggregatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_fused = 0;
        g_time_ev = 0;
        g_prio_high = 0;
        g_prio_low = 0;
    }
};

TEST_F(FebruarySensorAggregatorTest, TimeContextClassification) {
    auto deep = SensorAggregator::make_time_context(2, 30, 1);
    EXPECT_EQ(deep.tod, TimeOfDay::DeepNight);
    EXPECT_EQ(deep.day, DayClass::Workday);

    auto morning = SensorAggregator::make_time_context(8, 0, 0);
    EXPECT_EQ(morning.tod, TimeOfDay::Morning);

    auto weekend = SensorAggregator::make_time_context(10, 0, 6);
    EXPECT_EQ(weekend.day, DayClass::Weekend);
}

TEST_F(FebruarySensorAggregatorTest, SubscriptionPriorityOrdering) {
    EventBus::instance().clear();
    EventBus::instance().subscribe(EventType::SystemTick, on_low, nullptr, SubPriority::Low);
    EventBus::instance().subscribe(EventType::SystemTick, on_high, nullptr, SubPriority::High);

    Event tick;
    tick.type = EventType::SystemTick;
    tick.timestamp_ms = 1;
    EventBus::instance().publish(tick);
    EventBus::instance().process(4);

    EXPECT_EQ(g_prio_high, 1);
    EXPECT_EQ(g_prio_low, 1);
}

TEST_F(FebruarySensorAggregatorTest, SensorFusionUpdatesContext) {
    EventBus::instance().clear();
    SensorAggregator::instance().clear();
    ContextManager::instance().clear();

    EventBus::instance().subscribe(EventType::SensorFused, on_fused, nullptr, SubPriority::Normal);
    EventBus::instance().subscribe(EventType::TimeContextChanged, on_time, nullptr, SubPriority::Normal);

    auto& agg = SensorAggregator::instance();
    uint32_t t = 1000;
    EXPECT_TRUE(agg.feed_accel(100, 200, 1000, 180, t));
    EXPECT_TRUE(agg.feed_activity(ActivityState::Walking, 200, t + 1));
    EXPECT_TRUE(agg.feed_heart_rate(72, 210, t + 2));
    EXPECT_TRUE(agg.feed_time(8, 15, 0, 255, t + 3));
    EXPECT_TRUE(agg.feed_posture(15, -5, true, 190, t + 4));
    EXPECT_TRUE(agg.feed_ble_rssi(-55, true, 200, t + 5));
    EXPECT_TRUE(agg.feed_wifi_scan(3, -60, 170, t + 6));
    EXPECT_TRUE(agg.feed_rf_interference(40, -80 * 256, 180, t + 7));
    EXPECT_TRUE(agg.feed_steps(1200, 220, t + 8));
    EXPECT_TRUE(agg.feed_battery(88, 255, t + 9));

    while (EventBus::instance().process(16) > 0) {
    }

    EXPECT_GE(g_fused, 10);
    EXPECT_GE(g_time_ev, 1);

    const UserContext& ctx = ContextManager::instance().get();
    EXPECT_EQ(ctx.heart_rate, 72);
    EXPECT_EQ(ctx.battery_pct, 88);
    EXPECT_EQ(ctx.steps, 1200);
    EXPECT_TRUE(ctx.ble_connected);
    EXPECT_EQ(ctx.time_ctx.tod, TimeOfDay::Morning);
}

TEST_F(FebruarySensorAggregatorTest, CoreNightTimeContext) {
    FebruaryCore& f = FebruaryCore::instance();
    f.init();
    uint32_t t = 1000;
    f.feed_time(2, 0, 2, t + 200);
    f.feed_heart_rate(100, t + 201);
    f.process_events(32);
    EXPECT_EQ(f.context().time_ctx.tod, TimeOfDay::DeepNight);
}
