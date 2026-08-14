/**
 * @file service.hpp
 * @brief February OS-level service task (Phase 2.2)
 *
 * Platform-agnostic always-on loop:
 *   FebruaryService::instance().start();
 *   FebruaryService::instance().run_once(now_ms);
 *
 * States: Stopped -> Running <-> Suspended
 *
 * run_once:
 *   1) Optional: drain SoftBus remote (yields if local intent pending)
 *   2) tick + process local events
 *   3) If remote was deferred, inject after local
 */
#ifndef AURORA_FEBRUARY_SERVICE_HPP
#define AURORA_FEBRUARY_SERVICE_HPP

#include "config.hpp"
#include "types.hpp"
#include "february_core.hpp"
#include "softbus_stub.hpp"
#include "softbus.hpp"
#include "softbus_transport.hpp"
#include "peer_table.hpp"
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

    void bind_softbus_transport(const SoftBusTransportOps& ops) {
#if FEBRUARY_ENABLE_SOFTBUS
        SoftBus::instance().bind_transport(ops);
#else
        (void)ops;
#endif
    }

    unsigned run_once(uint32_t now_ms,
                      unsigned max_events = FEBRUARY_SERVICE_MAX_EVENTS) {
        if (state_ != ServiceState::Running) {
            return 0;
        }

        auto& core = FebruaryCore::instance();

#if FEBRUARY_ENABLE_SOFTBUS
        const bool yield =
#if FEBRUARY_REMOTE_YIELD_TO_LOCAL
            EventBus::instance().has_local_intent();
#else
            false;
#endif
        if (!yield) {
            SoftBus::instance().drain(max_events, [&](const SoftBusMessage& msg) {
                inject_remote(msg, now_ms, core);
            });
        }
#endif

        core.tick(now_ms);
        const unsigned handled = core.process_events(max_events);

#if FEBRUARY_ENABLE_SOFTBUS && FEBRUARY_REMOTE_YIELD_TO_LOCAL
        if (yield) {
            SoftBus::instance().drain(max_events, [&](const SoftBusMessage& msg) {
                inject_remote(msg, now_ms, core);
            });
            return handled + core.process_events(max_events);
        }
#endif
        return handled;
    }

    unsigned feed_and_run(uint32_t now_ms) { return run_once(now_ms); }

    void set_capability_hooks(const CapabilityHooks& h) {
        caps_ = h;
        ActionHooks ah;
        ah.on_speak = h.on_speak;
        ah.on_notify = h.on_notify;
        ah.on_set_dnd = h.on_set_dnd;
        ah.on_set_power = h.on_set_power;
        ah.on_transition_app = h.on_transition_app;
        ah.on_log = h.on_log;
        ah.user = h.user;
        FebruaryCore::instance().set_action_hooks(ah);
    }

    const CapabilityHooks& capability_hooks() const { return caps_; }

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

#if FEBRUARY_ENABLE_SOFTBUS
    void inject_remote(const SoftBusMessage& msg, uint32_t now_ms,
                       FebruaryCore& core) {
        Intent in = msg.intent;
        if (!in.valid()) {
            return;
        }
        if (msg.peer_id) {
            in.source_id = msg.peer_id;
        }
        Event ev;
        ev.type = EventType::RemoteIntent;
        ev.timestamp_ms = msg.timestamp_ms ? msg.timestamp_ms : now_ms;
        ev.source_id = msg.peer_id;
        ev.payload.intent = in;
        EventBus::instance().publish(ev);
        core.inject_intent(in, ev.timestamp_ms);
        PeerTable::instance().note_rx(msg.peer_id, ev.timestamp_ms);
        FEBRUARY_LOG("service: remote intent injected");
    }
#endif

    ServiceState    state_ = ServiceState::Stopped;
    CapabilityHooks caps_{};
};

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_SERVICE_HPP
