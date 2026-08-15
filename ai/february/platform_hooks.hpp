/**
 * @file platform_hooks.hpp
 * @brief Platform-agnostic capability hooks for February
 *
 * Any Aurora node (watch, gateway, phone companion, RV32 board) binds
 * the same interface to local drivers / SoftBus / UI. Phase 1 ActionHooks
 * remains for compatibility; prefer CapabilityHooks for new integration.
 *
 * No heap. All callbacks optional (null = no-op).
 */
#ifndef AURORA_FEBRUARY_PLATFORM_HOOKS_HPP
#define AURORA_FEBRUARY_PLATFORM_HOOKS_HPP

#include "types.hpp"
#include <cstdint>

namespace aurora {
namespace february {

/**
 * Capability surface — map to real OS services at integration time.
 *
 * Examples:
 *   Watch:   on_speak → TTS; on_transition_app → AppControlBlock
 *   Gateway: on_speak → SoftBus notify; on_publish_remote → SoftBus
 *   Phone:   on_notify → system notification; on_set_dnd → focus API
 */
struct CapabilityHooks {
    // Local output
    void (*on_speak)(const char* msg, void* user) = nullptr;
    void (*on_notify)(const char* msg, void* user) = nullptr;
    void (*on_set_dnd)(bool enable, void* user) = nullptr;
    void (*on_set_power)(int32_t mode, void* user) = nullptr;
    void (*on_transition_app)(int32_t app_id, int32_t state, void* user) = nullptr;
    void (*on_log)(const char* msg, void* user) = nullptr;

    // Cross-device (SoftBus / peer)
    // payload is opaque short message; peer_id 0 = broadcast / local bus
    void (*on_publish_remote)(uint32_t peer_id, const Intent* intent, void* user) = nullptr;
    void (*on_capability_query)(uint32_t peer_id, void* user) = nullptr;

    void* user = nullptr;
};

/** Convert Phase-1 ActionHooks into CapabilityHooks (shared fields only). */
struct ActionHooks;  // forward from action_executor.hpp

inline CapabilityHooks from_action_hooks(
    void (*on_speak)(const char*, void*),
    void (*on_notify)(const char*, void*),
    void (*on_set_dnd)(bool, void*),
    void (*on_transition_app)(int32_t, int32_t, void*),
    void (*on_log)(const char*, void*),
    void* user) {
    CapabilityHooks c;
    c.on_speak = on_speak;
    c.on_notify = on_notify;
    c.on_set_dnd = on_set_dnd;
    c.on_transition_app = on_transition_app;
    c.on_log = on_log;
    c.user = user;
    return c;
}

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_PLATFORM_HOOKS_HPP
