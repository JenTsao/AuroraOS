#include "ai/february/february_core.hpp"
#include "ai/february/sensor_aggregator.hpp"
#include <cstdio>
#include <cassert>
using namespace aurora::february;
static int g_fused=0, g_time_ev=0, g_prio_high=0, g_prio_low=0;
static void on_fused(const Event& ev, void*) {
  if (ev.type==EventType::SensorFused) { ++g_fused; assert(ev.payload.sensor.confidence_q8<=255); }
}
static void on_time(const Event& ev, void*) {
  if (ev.type==EventType::TimeContextChanged) ++g_time_ev;
}
static void on_high(const Event&, void*) { ++g_prio_high; }
static void on_low(const Event&, void*) { assert(g_prio_high>=1); ++g_prio_low; }
int main() {
  printf("=== SensorAggregator test ===\n");
  EventBus::instance().clear();
  SensorAggregator::instance().clear();
  ContextManager::instance().clear();
  auto deep = SensorAggregator::make_time_context(2,30,1);
  assert(deep.tod==TimeOfDay::DeepNight && deep.day==DayClass::Workday);
  auto morning = SensorAggregator::make_time_context(8,0,0);
  assert(morning.tod==TimeOfDay::Morning);
  auto weekend = SensorAggregator::make_time_context(10,0,6);
  assert(weekend.day==DayClass::Weekend);
  EventBus::instance().subscribe(EventType::SystemTick, on_low, nullptr, SubPriority::Low);
  EventBus::instance().subscribe(EventType::SystemTick, on_high, nullptr, SubPriority::High);
  Event tick; tick.type=EventType::SystemTick; tick.timestamp_ms=1;
  EventBus::instance().publish(tick);
  EventBus::instance().process(4);
  assert(g_prio_high==1 && g_prio_low==1);
  EventBus::instance().clear();
  SensorAggregator::instance().clear();
  ContextManager::instance().clear();
  g_fused=g_time_ev=0;
  EventBus::instance().subscribe(EventType::SensorFused, on_fused, nullptr, SubPriority::Normal);
  EventBus::instance().subscribe(EventType::TimeContextChanged, on_time, nullptr, SubPriority::Normal);
  auto& agg = SensorAggregator::instance();
  uint32_t t=1000;
  assert(agg.feed_accel(100,200,1000,180,t));
  assert(agg.feed_activity(ActivityState::Walking,200,t+1));
  assert(agg.feed_heart_rate(72,210,t+2));
  assert(agg.feed_time(8,15,0,255,t+3));
  assert(agg.feed_posture(15,-5,true,190,t+4));
  assert(agg.feed_ble_rssi(-55,true,200,t+5));
  assert(agg.feed_wifi_scan(3,-60,170,t+6));
  assert(agg.feed_rf_interference(40,-80*256,180,t+7));
  assert(agg.feed_steps(1200,220,t+8));
  assert(agg.feed_battery(88,255,t+9));
  while (EventBus::instance().process(16)>0) {}
  assert(g_fused>=10 && g_time_ev>=1);
  const UserContext& ctx = ContextManager::instance().get();
  assert(ctx.heart_rate==72 && ctx.battery_pct==88 && ctx.steps==1200);
  assert(ctx.ble_connected && ctx.time_ctx.tod==TimeOfDay::Morning);
  FebruaryCore& f = FebruaryCore::instance();
  f.init();
  f.feed_time(2,0,2,t+200);
  f.feed_heart_rate(100,t+201);
  f.process_events(32);
  assert(f.context().time_ctx.tod==TimeOfDay::DeepNight);
  printf("ALL SensorAggregator tests passed\n");
  return 0;
}
