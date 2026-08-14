/**
 * @file service.hpp
 * @brief February OS-level service task (Phase 2)
 *
 * Platform-agnostic always-on loop. Not tied to watch daemon or any
 * single device class. Host / RTOS integration calls:
 *
 *   FebruaryService::instance().start();
 *   // in task or main loop:
 *   FebruaryService::instance().run_once(now_ms);
 *
 * States: Stopped → Running ↔ Suspended
 *
 * run_once does: SoftBus drain → tick → process_events
 * (planner path is handled inside core intent subscription).
 */
#ifndef AURORA_FEBRUARY_SERVICE_HPP
#define AURORA_FEBRUARY_SERVICE_HPP

#include "config.hpp"
#include "types.hpp"
#include "february_core.hpp"
#include "softbus_stub.hpp"
#include "softbus.hpp"
#include "softbus_transport.hpp"
#include "platform_hooks.hpp"
#include "log.hpp"

namespace aurora {
namespace february {

enum class ServiceState : uint8_t {
    Stopped = 0,
    Running,
    Suspended
};

class FebruaryService {
public:
    static FebruaryService& instance() {
        static FebruaryService svc;
        return svc;
    }

    ServiceState state() const { return state_; }

    bool start() {
        auto& core = FebruaryCore::instance();
        if (!core.ready()) {
            core.init();
        }
#if FEBRUARY_ENABLE_SOFTBUS
        SoftBus::instance().clear();
        SoftBus::instance().start_server();
#endif
        state_ = ServiceState::Running;
        FEBRUARY_LOG("service: start");
        return true;
    }

    void stop() {
#if FEBRUARY_ENABLE_SOFTBUS
        SoftBus::instance().stop_server();
#endif
        state_ = ServiceState::Stopped;
        FEBRUARY_LOG("service: stop");
    }

    void suspend() {
        if (state_ == ServiceState::Running) {
            state_ = ServiceState::Suspended;
            FEBRUARY_LOG("service: suspend");
        }
    }

    void resume() {
        if (state_ == ServiceState::Suspended) {
            state_ = ServiceState::Running;
            FEBRUARY_LOG("service: resume");
        }
    }

    /**
     * Bind real SoftBus / mock transport before or after start().
     * Example: SoftBus::instance().bind_transport(OhSoftBusAdapter::instance().ops());
     */
    void bind_softbus_transport(const SoftBusTransportOps& ops) {
#if FEBRUARY_ENABLE_SOFTBUS
        SoftBus::instance().bind_transport(ops);
#else
        (void)ops;
#endif
    }

    /**
     * One service iteration. Call from RTOS task or host loop (~10–100 Hz ok).
     * Returns number of events processed.
     */
    unsigned run_once(uint32_t now_ms, unsigned max_events = FEBRUARY_SERVICE_MAX_EVENTS) {
        if (state_ != ServiceState::Running) {
            return 0;
        }

        auto& core = FebruaryCore::instance();

        // 1) Drain SoftBus remote intents into core
#if FEBRUARY_ENABLE_SOFTBUS
        SoftBus::instance().drain(max_events, [&](const SoftBusMessage& msg) {
            Intent in = msg.intent;
            if (!in.valid()) {
                return;
            }
            Event ev;
            ev.type = EventType::RemoteIntent;
            ev.timestamp_ms = msg.timestamp_ms ? msg.timestamp_ms : now_ms;
            ev.source_id = msg.peer_id;
            ev.payload.intent = in;
            EventBus::instance().publish(ev);
            core.inject_intent(in, ev.timestamp_ms);
            FEBRUARY_LOG("service: remote intent injected");
        });
#endif

        // 2) Time + context
        core.tick(now_ms);

        // 3) Drain local event bus
        return core.process_events(max_events);
    }

    unsigned feed_and_run(uint32_t now_ms) {
        return run_once(now_ms);
    }

    void set_capability_hooks(const CapabilityHooks& h) {
        caps_ = h;
        ActionHooks ah;
        ah.on_speak = h.on_speak;
        ah.on_notify = h.on_notify;
        ah.on_set_dnd = h.on_set_dnd;
        ah.on_transition_app = h.on_transition_app;
        ah.on_log = h.on_log;
        ah.user = h.user;
        FebruaryCore::instance().set_action_hooks(ah);
    }

    const CapabilityHooks& capability_hooks() const { return caps_; }

    /**
     * Publish intent to a peer over SoftBus (transport + local inbox).
     * peer_id 0 = local loopback only.
     */
    void publish_remote(uint32_t peer_id, const Intent& in, uint32_t now_ms) {
#if FEBRUARY_ENABLE_SOFTBUS
        if (caps_.on_publish_remote) {
            caps_.on_publish_remote(peer_id, &in, caps_.user);
        }
        SoftBus::instance().publish_intent(peer_id, in, now_ms,
                                           /*loopback=*/peer_id == 0);
#else
        (void)peer_id;
        (void)in;
        (void)now_ms;
#endif
    }

private:
    FebruaryService() = default;

    ServiceState     state_ = ServiceState::Stopped;
    CapabilityHooks  caps_{};
};

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_SERVICE_HPP
