/**
 * @file firmware_bridge.hpp
 * @brief Firmware-level closed-loop bridge for February AI Engine
 *
 * Connects FebruaryService / FebruaryCore to AuroraOS board services on both
 * lm3s6965-qb (system_daemon_task / kernel_main) and miband8 (WatchApp):
 *   1. Sensor Ingestion: steps, heart rate, battery SoC, wrist gesture
 *   2. Capability Execution:
 *      - NotifyUser / Speak -> NotificationCenter (NotificationOverlayView)
 *      - SetPower           -> PowerManager 5-state transition
 *      - TransitionApp      -> AppControlBlock (FOREGROUND / BACKGROUND)
 *      - SetDnd             -> Do-Not-Disturb gating for non-critical alerts
 *      - PublishRemote      -> Outbound SoftBus binary intent frame
 *   3. DistributedSoftBus Bridge:
 *      - Outbound SoftBusTransportOps -> DistributedSoftBus::send_raw_frame
 *      - Inbound UDP 8899 0xFB02 frame -> SoftBus::on_bytes_received ->
 *        FebruaryService::run_once drains & executes remote intent locally
 *      - Authenticated beacon peer discovery -> SoftBus::register_peer
 *
 * Header-only, zero heap allocation, noexcept, freestanding C++17 safe.
 */
#ifndef AURORA_FEBRUARY_FIRMWARE_BRIDGE_HPP
#define AURORA_FEBRUARY_FIRMWARE_BRIDGE_HPP

#include "board_bind.hpp"
#include "crit.hpp"
#include "february_core.hpp"
#include "platform_hooks.hpp"
#include "service.hpp"
#include "softbus.hpp"
#include "softbus_transport.hpp"
#include "string_util.hpp"

#include "../../drivers/power/charging_manager.hpp"
#include "../../drivers/sensor/sensor_framework.hpp"
#include "../../kernel/core/arch_api.hpp"
#include "../../kernel/core/power/power_manager.hpp"
#include "../../kernel/task/app_lifecycle.hpp"

namespace aurora {
namespace february {

using FirmwareNotifyFn = void (*)(const char* title, const char* body,
                                  bool is_critical, uint32_t now_ms,
                                  void* user);

class FirmwareBridge {
public:
    static FirmwareBridge& instance() noexcept {
        static FirmwareBridge inst;
        return inst;
    }

    /**
     * Initialize February AI service, critical section hooks, capability
     * callbacks, SoftBus transport, and optional AppControlBlock binding.
     */
    bool init(AppControlBlock* app = nullptr,
              FirmwareNotifyFn notify_sink = nullptr,
              void* notify_user = nullptr) noexcept {
        bound_app_ = app;
        if (notify_sink) {
            notify_sink_ = notify_sink;
            notify_user_ = notify_user;
        }
#if defined(AURORA_NOTIFICATION_CENTER_HPP)
        if (!notify_sink_) {
            notify_sink_ = &FirmwareBridge::default_notification_center_sink;
            notify_user_ = this;
        }
#endif

        last_steps_ = 0;
        idle_counts_ = 0;
        last_step_feed_ms_ = 0;
        current_now_ms_ = 0;
        dnd_active_ = false;
        wrist_latched_ = false;
        notify_count_ = 0;
        speak_count_ = 0;
        power_transition_count_ = 0;
        app_transition_count_ = 0;
        remote_publish_count_ = 0;
        last_spoken_[0] = '\0';
        last_notified_[0] = '\0';

        hooks_ = CapabilityHooks{};
        hooks_.on_speak = &FirmwareBridge::on_speak_cb;
        hooks_.on_notify = &FirmwareBridge::on_notify_cb;
        hooks_.on_set_dnd = &FirmwareBridge::on_set_dnd_cb;
        hooks_.on_set_power = &FirmwareBridge::on_set_power_cb;
        hooks_.on_transition_app = &FirmwareBridge::on_transition_app_cb;
        hooks_.on_publish_remote = &FirmwareBridge::on_publish_remote_cb;
        hooks_.user = this;

        softbus_ops_ = SoftBusTransportOps{};
        softbus_ops_.create_server = &FirmwareBridge::sb_create_server;
        softbus_ops_.remove_server = &FirmwareBridge::sb_remove_server;
        softbus_ops_.open_session = &FirmwareBridge::sb_open_session;
        softbus_ops_.close_session = &FirmwareBridge::sb_close_session;
        softbus_ops_.send_bytes = &FirmwareBridge::sb_send_bytes;
        softbus_ops_.set_rx_sink = &FirmwareBridge::sb_set_rx_sink;
        softbus_ops_.user = this;

#if defined(AURORA_DISTRIBUTED_BUS_HPP)
        bind_distributed_softbus();
#endif

        FebruaryCore::instance().reset();
        SoftBus::instance().clear();

        BoardBindArgs args{};
        args.crit_enter = &FirmwareBridge::crit_enter;
        args.crit_exit = &FirmwareBridge::crit_exit;
        args.crit_user = this;
        args.caps = &hooks_;
#if FEBRUARY_ENABLE_SOFTBUS
        args.transport = &softbus_ops_;
#endif
        args.wake_word = nullptr;

        initialized_ = board_bind_start(args);
        notify_seq_ = 1;
        last_hour_ = 10;
        last_minute_ = 9;
        // Default daytime clock context (10:09 workday) unless overridden by
        // watch RTC / simulated clock via step_watch() or feed_time().
        FebruaryCore::instance().feed_time(10, 9, 1, 0);
        return initialized_;
    }

    bool init_watch() noexcept {
        return init(nullptr, nullptr, nullptr);
    }

#if defined(AURORA_DISTRIBUTED_BUS_HPP)
    void bind_distributed_softbus() noexcept {
        DistributedSoftBus::instance().set_intent_frame_handler(
            &FirmwareBridge::on_distributed_intent_frame, this);
        DistributedSoftBus::instance().set_peer_discovery_handler(
            &FirmwareBridge::on_distributed_peer_discovered, this);
    }
#endif

    void set_notify_sink(FirmwareNotifyFn fn, void* user = nullptr) noexcept {
        notify_sink_ = fn;
        notify_user_ = user;
    }

    void set_bound_app(AppControlBlock* app) noexcept {
        bound_app_ = app;
    }

    /**
     * System daemon periodic step (lm3s6965-qb / kernel.cpp system_daemon_task).
     * Polls SensorManager + ChargingManager, drives legacy AppControlBlock
     * step-activity promotion/demotion, and runs February AI closed loop.
     */
    unsigned step_daemon(uint32_t now_ms) noexcept {
        if (!initialized_ || FebruaryService::instance().state() != ServiceState::Running) {
            init(bound_app_, notify_sink_, notify_user_);
        }

        uint32_t steps = last_steps_;
        uint16_t bpm = 0;
        uint8_t battery = 100;
        bool wrist = false;

#if !defined(CONFIG_BOARD_LM3S6965_QB)
        steps = SensorManager::instance().get_accel_sensor().get_steps();
        SensorData data{};
        if (SensorManager::instance().pop_data(&data) &&
            data.type == SensorType::HEART_RATE) {
            bpm = static_cast<uint16_t>(data.payload.bpm);
        }
        battery = ChargingManager::instance().get_soc();
        wrist = (PowerManager::instance().get_state() == PowerState::ACTIVE) &&
                (PowerManager::instance().get_last_wake_reason() ==
                 WakeReason::WRIST_RAISE);
#endif

        // Maintain deterministic AppControlBlock lifecycle for step activity
        if (bound_app_) {
            if (steps != last_steps_) {
                idle_counts_ = 0;
                if (bound_app_->state != AppState::FOREGROUND) {
                    bound_app_->transition_to(AppState::FOREGROUND);
                }
            } else {
                ++idle_counts_;
                if (idle_counts_ > 10 &&
                    bound_app_->state == AppState::FOREGROUND) {
                    bound_app_->transition_to(AppState::BACKGROUND);
                }
            }
        }

        return step_sensors(now_ms, steps, bpm, battery, wrist);
    }

    /**
     * Watch daemon periodic step (miband8 WatchApp::on_background_tick, 40ms).
     */
    unsigned step_watch(uint32_t now_ms, uint32_t steps, uint16_t bpm,
                        uint8_t battery_pct, uint8_t hour = 10,
                        uint8_t minute = 9) noexcept {
        if (!initialized_ || FebruaryService::instance().state() != ServiceState::Running) {
            init_watch();
        }

        if (hour != last_hour_ || minute != last_minute_) {
            last_hour_ = hour;
            last_minute_ = minute;
            FebruaryCore::instance().feed_time(hour, minute, 1, now_ms);
        }

        const bool wrist =
            (PowerManager::instance().get_state() == PowerState::ACTIVE) &&
            (PowerManager::instance().get_last_wake_reason() ==
             WakeReason::WRIST_RAISE) &&
            (PowerManager::instance().get_state_elapsed_ms() < 500u);

        return step_sensors(now_ms, steps, bpm, battery_pct, wrist);
    }

    /**
     * Unified sensor + SoftBus + EventBus step for a single timestamp.
     */
    unsigned step_sensors(uint32_t now_ms, uint32_t steps, uint16_t bpm,
                          uint8_t battery_pct, bool wrist_raised) noexcept {
        current_now_ms_ = now_ms;
        auto& core = FebruaryCore::instance();
        if (!core.ready()) {
            core.init();
        }

        // Advance context idle timer before checking step-based rest triggers
        ContextManager::instance().tick(now_ms);

        core.feed_battery(battery_pct, now_ms);
        if (bpm > 0) {
            core.feed_heart_rate(bpm, now_ms);
        }

        if (steps != last_steps_ || (now_ms - last_step_feed_ms_) >= 500u ||
            last_step_feed_ms_ == 0u) {
            last_steps_ = steps;
            last_step_feed_ms_ = now_ms;
            core.feed_steps(steps, now_ms);
        }

        if (wrist_raised != wrist_latched_) {
            wrist_latched_ = wrist_raised;
            core.feed_wrist_gesture(wrist_raised, now_ms);
        }

        return FebruaryService::instance().run_once(now_ms);
    }

    [[nodiscard]] bool dnd_active() const noexcept { return dnd_active_; }
    [[nodiscard]] uint32_t notify_count() const noexcept { return notify_count_; }
    [[nodiscard]] uint32_t speak_count() const noexcept { return speak_count_; }
    [[nodiscard]] uint32_t power_transition_count() const noexcept {
        return power_transition_count_;
    }
    [[nodiscard]] uint32_t app_transition_count() const noexcept {
        return app_transition_count_;
    }
    [[nodiscard]] uint32_t remote_publish_count() const noexcept {
        return remote_publish_count_;
    }
    [[nodiscard]] const char* last_spoken() const noexcept {
        return last_spoken_;
    }
    [[nodiscard]] const char* last_notified() const noexcept {
        return last_notified_;
    }

    static uint32_t hash_device_id(const char* dev_id) noexcept {
        if (!dev_id || !dev_id[0]) {
            return 1u;
        }
        uint32_t h = 2166136261u;
        while (*dev_id) {
            h ^= static_cast<uint8_t>(*dev_id++);
            h *= 16777619u;
        }
        return (h == 0u) ? 1u : h;
    }

private:
    FirmwareBridge() noexcept = default;

    static void crit_enter(void* user) noexcept {
        auto* self = static_cast<FirmwareBridge*>(user);
        if (!self) return;
        const uint32_t flags = Arch::irq_save();
        if (self->crit_depth_ == 0) {
            self->saved_irq_ = flags;
        }
        ++self->crit_depth_;
    }

    static void crit_exit(void* user) noexcept {
        auto* self = static_cast<FirmwareBridge*>(user);
        if (!self || self->crit_depth_ == 0) return;
        --self->crit_depth_;
        if (self->crit_depth_ == 0) {
            Arch::irq_restore(self->saved_irq_);
        }
    }

    static bool is_critical_message(const char* msg) noexcept {
        const IntentType last =
            FebruaryCore::instance().memory().last_intent().type;
        if (last == IntentType::Emergency || last == IntentType::BatteryLow) {
            return true;
        }
        return contains_ci(msg, "Emergency") || contains_ci(msg, "Battery low");
    }

    static void on_notify_cb(const char* msg, void* user) noexcept {
        auto* self = static_cast<FirmwareBridge*>(user);
        if (!self || !msg) return;
        copy_cstr(self->last_notified_, sizeof(self->last_notified_), msg);
        ++self->notify_count_;

        const bool critical = is_critical_message(msg);
        if (self->dnd_active_ && !critical) {
            return;
        }
        if (self->notify_sink_) {
            self->notify_sink_("February AI", msg, critical,
                               self->current_now_ms_, self->notify_user_);
        }
#if !defined(CONFIG_BOARD_LM3S6965_QB)
        if (!critical &&
            PowerManager::instance().get_state() != PowerState::CRITICAL) {
            PowerManager::instance().notify_external_wakeup();
        }
#endif
    }

    static void on_speak_cb(const char* msg, void* user) noexcept {
        auto* self = static_cast<FirmwareBridge*>(user);
        if (!self || !msg) return;
        copy_cstr(self->last_spoken_, sizeof(self->last_spoken_), msg);
        ++self->speak_count_;

        if (self->dnd_active_) {
            return;
        }
        // Avoid duplicate banner when the same plan already emitted Emergency
        // NotifyUser immediately before Speak.
        const IntentType last =
            FebruaryCore::instance().memory().last_intent().type;
        if (last == IntentType::Emergency) {
            return;
        }
        if (self->notify_sink_) {
            self->notify_sink_("February AI", msg, false, self->current_now_ms_,
                               self->notify_user_);
        }
    }

    static void on_set_dnd_cb(bool enable, void* user) noexcept {
        auto* self = static_cast<FirmwareBridge*>(user);
        if (!self) return;
        self->dnd_active_ = enable;
    }

    static void on_set_power_cb(int32_t mode, void* user) noexcept {
        auto* self = static_cast<FirmwareBridge*>(user);
        if (self) {
            ++self->power_transition_count_;
        }
#if !defined(CONFIG_BOARD_LM3S6965_QB)
        PowerState target = PowerState::ACTIVE;
        switch (static_cast<PowerMode>(mode)) {
        case PowerMode::Active:
            target = PowerState::ACTIVE;
            break;
        case PowerMode::Dim:
            target = PowerState::DIM;
            break;
        case PowerMode::Idle:
            target = PowerState::IDLE;
            break;
        case PowerMode::Sleep:
            target = PowerState::SLEEP;
            break;
        case PowerMode::Critical:
            target = PowerState::CRITICAL;
            break;
        }
        PowerManager::instance().transition_to(target);
#else
        (void)mode;
#endif
    }

    static void on_transition_app_cb(int32_t /*app_id*/, int32_t state,
                                     void* user) noexcept {
        auto* self = static_cast<FirmwareBridge*>(user);
        if (!self) return;
        ++self->app_transition_count_;
        if (!self->bound_app_) return;
        if (state != 0) {
            if (self->bound_app_->state != AppState::FOREGROUND) {
                self->bound_app_->transition_to(AppState::FOREGROUND);
            }
        } else {
            if (self->bound_app_->state == AppState::FOREGROUND) {
                self->bound_app_->transition_to(AppState::BACKGROUND);
            }
        }
    }

    static void on_publish_remote_cb(uint32_t peer_id, const Intent* intent,
                                     void* user) noexcept {
        auto* self = static_cast<FirmwareBridge*>(user);
        if (!self || !intent) return;
        ++self->remote_publish_count_;
        SoftBus::instance().publish_intent(peer_id, *intent,
                                           self->current_now_ms_);
    }

#if defined(AURORA_NOTIFICATION_CENTER_HPP)
    static void default_notification_center_sink(const char* title,
                                                 const char* body,
                                                 bool is_critical,
                                                 uint32_t now_ms,
                                                 void* user) noexcept {
        auto* self = static_cast<FirmwareBridge*>(user);
        aurora::Notification n{};
        n.id = self ? self->notify_seq_++ : 1u;
        n.priority = is_critical ? aurora::NotificationPriority::critical
                                 : aurora::NotificationPriority::high;
        n.category = aurora::NotificationCategory::system;
        n.timestamp = now_ms;
        n.dismissed = false;
        copy_cstr(n.title, sizeof(n.title), title ? title : "February AI");
        copy_cstr(n.body, sizeof(n.body), body ? body : "");
        (void)aurora::NotificationCenter::instance().post(n);
    }
#endif

    // ── SoftBusTransportOps callbacks ──────────────────────────────────────
    static int sb_create_server(const char* /*pkg*/, const char* /*session*/,
                                void* /*user*/) noexcept {
        return 0;
    }

    static int sb_remove_server(const char* /*pkg*/, const char* /*session*/,
                                void* /*user*/) noexcept {
        return 0;
    }

    static SoftBusSessionId sb_open_session(const char* /*local_session*/,
                                            const char* /*peer_session*/,
                                            const char* peer_network_id,
                                            void* /*user*/) noexcept {
        if (!peer_network_id || !peer_network_id[0]) {
            return 1;
        }
        return static_cast<SoftBusSessionId>(
            (hash_device_id(peer_network_id) & 0x7FFFu) | 1u);
    }

    static int sb_close_session(SoftBusSessionId /*sid*/,
                                void* /*user*/) noexcept {
        return 0;
    }

    static int sb_send_bytes(SoftBusSessionId /*sid*/, const void* data,
                             unsigned len, void* /*user*/) noexcept {
        if (!data || len == 0) {
            return -1;
        }
#if defined(AURORA_DISTRIBUTED_BUS_HPP)
        const int sent = DistributedSoftBus::instance().send_raw_frame(data, len);
        return (sent >= 0) ? 0 : -1;
#else
        return 0;
#endif
    }

    static void sb_set_rx_sink(const SoftBusRxSink* sink,
                               void* user) noexcept {
        auto* self = static_cast<FirmwareBridge*>(user);
        if (self && sink) {
            self->rx_sink_ = *sink;
        }
    }

#if defined(AURORA_DISTRIBUTED_BUS_HPP)
    static void on_distributed_intent_frame(int session_id, const uint8_t* data,
                                            size_t len, void* /*user*/) noexcept {
        SoftBus::instance().on_bytes_received(
            static_cast<SoftBusSessionId>(session_id), data,
            static_cast<unsigned>(len));
    }

    static void on_distributed_peer_discovered(const char* device_id,
                                               const char* /*ip_str*/,
                                               uint32_t now_tick,
                                               void* /*user*/) noexcept {
        const uint32_t peer_id = hash_device_id(device_id);
        (void)SoftBus::instance().register_peer(peer_id, device_id, now_tick);
    }
#endif

    CapabilityHooks hooks_{};
    SoftBusTransportOps softbus_ops_{};
    SoftBusRxSink rx_sink_{};
    AppControlBlock* bound_app_ = nullptr;
    FirmwareNotifyFn notify_sink_ = nullptr;
    void* notify_user_ = nullptr;

    uint32_t last_steps_ = 0;
    uint32_t idle_counts_ = 0;
    uint32_t last_step_feed_ms_ = 0;
    uint32_t current_now_ms_ = 0;
    uint32_t notify_seq_ = 0;
    uint32_t saved_irq_ = 0;
    uint32_t crit_depth_ = 0;
    bool dnd_active_ = false;
    bool wrist_latched_ = false;
    bool initialized_ = false;
    uint8_t last_hour_ = 0;
    uint8_t last_minute_ = 0;

    static constexpr unsigned kMsgBufLen = 64;
    char last_spoken_[kMsgBufLen]{};
    char last_notified_[kMsgBufLen]{};
    uint32_t notify_count_ = 0;
    uint32_t speak_count_ = 0;
    uint32_t power_transition_count_ = 0;
    uint32_t app_transition_count_ = 0;
    uint32_t remote_publish_count_ = 0;
};

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_FIRMWARE_BRIDGE_HPP
