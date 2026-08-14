/**
 * @file board_bind.hpp
 * @brief Minimal board integration helpers for Phase 2.2
 *
 * Copy patterns into your RTOS task / main - this header is documentation
 * plus optional convenience wrappers. No OS symbols are required to compile.
 *
 * Typical boot (10 lines):
 *
 *   using namespace aurora::february;
 *   FebruaryCrit::set(my_enter, my_exit);          // optional
 *   OhSoftBusAdapter::instance().bind(oh_fns);     // optional real SoftBus
 *   auto& svc = FebruaryService::instance();
 *   svc.bind_softbus_transport(OhSoftBusAdapter::instance().ops());
 *   svc.set_capability_hooks(caps);
 *   FebruaryCore::instance().set_wake_word("hey february");
 *   svc.start();
 *   SoftBus::instance().register_peer(2, peerNetId, now_ms);
 *
 * Task loop:
 *   core.feed_steps(...);  // or battery / text
 *   svc.run_once(now_ms);
 */
#ifndef AURORA_FEBRUARY_BOARD_BIND_HPP
#define AURORA_FEBRUARY_BOARD_BIND_HPP

#include "config.hpp"
#include "crit.hpp"
#include "service.hpp"
#include "softbus.hpp"
#include "platform_hooks.hpp"

#if FEBRUARY_ENABLE_SOFTBUS
#include "softbus_oh_adapter.hpp"
#endif

namespace aurora {
namespace february {

struct BoardBindArgs {
    FebruaryCritFn       crit_enter = nullptr;
    FebruaryCritFn       crit_exit  = nullptr;
    void*                crit_user  = nullptr;
    const CapabilityHooks* caps     = nullptr;
    const char*          wake_word  = nullptr;
#if FEBRUARY_ENABLE_SOFTBUS
    const SoftBusTransportOps* transport = nullptr;
    uint32_t             local_peer_id = 1;
#endif
};

inline bool board_bind_start(const BoardBindArgs& a) {
    if (a.crit_enter || a.crit_exit) {
        FebruaryCrit::set(a.crit_enter, a.crit_exit, a.crit_user);
    }

    auto& svc = FebruaryService::instance();

#if FEBRUARY_ENABLE_SOFTBUS
    if (a.transport) {
        svc.bind_softbus_transport(*a.transport);
    }
#endif

    if (a.caps) {
        svc.set_capability_hooks(*a.caps);
    }

    if (a.wake_word) {
        FebruaryCore::instance().set_wake_word(a.wake_word);
    }

    if (!svc.start()) {
        return false;
    }

#if FEBRUARY_ENABLE_SOFTBUS
    SoftBus::instance().set_local_peer_id(a.local_peer_id);
#endif
    return true;
}

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_BOARD_BIND_HPP
