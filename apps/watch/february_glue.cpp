#include "february_glue.hpp"

// ============================================================
// apps/watch/february_glue.cpp — February 胶水层实现
//
// 只被 miband8 固件与 host 单元测试编译（两处都在各自构建里显式列出），
// 刻意不碰 SensorManager / ChargingManager / BLE —— 那些由
// miband_kernel.hpp 的 sensor_ble_daemon_task 取好后传进来，
// 这样本 TU 无任何硬件依赖，host 侧可完整覆盖。
// ============================================================

namespace aurora {
namespace watch {

namespace {

// 上一次喂进去的值，用于「是否变化」判定。
// static：本 TU 只有 sensor_ble_daemon_task 一个调用者，且 February 自身
// 是单消费者；不需要跨 TU 共享。
FebSensorSample g_last{};
bool g_booted = false;

} // namespace

bool february_boot() noexcept {
#if FEBRUARY_COMPILED
    if (g_booted) {
        return true;
    }
    // FebruaryCore::init() 由 FebruaryService::start() 内部兜底调用。
    // SoftBus 本阶段不 bind transport —— 意图只在本机推理，不跨设备。
    february::FebruaryService::instance().start();
    g_booted = true;
    return true;
#else
    // CONFIG_FEBRUARY 未开启（Kconfig 里关掉 February）：整个子系统不编入，所有接口退化为 no-op。
    // 调用方因此不必写 #ifdef。
    return false;
#endif
}

void february_bind_critical_section(void (*enter)(void*), void (*exit)(void*)) noexcept {
#if FEBRUARY_COMPILED
    february::FebruaryCrit::set(enter, exit, nullptr);
#else
    (void)enter;
    (void)exit;
#endif
}

void february_feed(const FebSensorSample& s) noexcept {
#if FEBRUARY_COMPILED
    auto& core = february::FebruaryCore::instance();
    if (!core.ready()) {
        // 未 boot 就喂：FebruaryCore 会拒收。静默返回，避免在启动竞态里
        // 污染它的内部状态。
        return;
    }

    // 步数：February 自己做差分，只需在累计值变化时喂。
    if (s.steps != g_last.steps) {
        core.feed_steps(s.steps, 0);
        g_last.steps = s.steps;
    }

    if (s.battery_pct != g_last.battery_pct) {
        core.feed_battery(s.battery_pct, 0);
        g_last.battery_pct = s.battery_pct;
    }

    // 心率：0 表示「本周期无采样」，不是「心率为 0」。必须跳过，
    // 否则 February 会把设备判成心跳停止。
    if (s.heart_rate != 0 && s.heart_rate != g_last.heart_rate) {
        core.feed_heart_rate(s.heart_rate, 0);
        g_last.heart_rate = s.heart_rate;
    }

    if (s.hour != g_last.hour || s.minute != g_last.minute || s.weekday != g_last.weekday) {
        core.feed_time(s.hour, s.minute, s.weekday, 0);
        g_last.hour = s.hour;
        g_last.minute = s.minute;
        g_last.weekday = s.weekday;
    }

    if (s.ble_connected != g_last.ble_connected || s.ble_rssi_dbm != g_last.ble_rssi_dbm) {
        core.feed_ble_rssi(s.ble_rssi_dbm, s.ble_connected, 0);
        g_last.ble_connected = s.ble_connected;
        g_last.ble_rssi_dbm = s.ble_rssi_dbm;
    }
#else
    (void)s;
#endif
}

uint32_t february_tick(uint32_t now_ms) noexcept {
#if FEBRUARY_COMPILED
    if (!g_booted) {
        return 0;
    }
    return static_cast<uint32_t>(february::FebruaryService::instance().run_once(now_ms));
#else
    (void)now_ms;
    return 0;
#endif
}

uint32_t february_total_intents() noexcept {
#if FEBRUARY_COMPILED
    return february::FebruaryCore::instance().total_intents_processed();
#else
    return 0;
#endif
}

bool february_is_running() noexcept {
#if FEBRUARY_COMPILED
    return g_booted && february::FebruaryService::instance().state() == february::ServiceState::Running;
#else
    return false;
#endif
}

} // namespace watch
} // namespace aurora
