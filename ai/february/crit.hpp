/**
 * @file crit.hpp
 * @brief Optional critical-section hooks for February (Phase 2.2)
 *
 * EventBus / SoftBus queues are single-producer / single-consumer by default.
 * On RTOS boards where ISR or another task may publish concurrently, bind:
 *
 *   FebruaryCrit::set(enter_fn, exit_fn, user);
 *
 * Typical binding: disable IRQ, take a mutex, or call OS critical enter/exit.
 * Null hooks = no-op (host tests, single-thread loops).
 */
#ifndef AURORA_FEBRUARY_CRIT_HPP
#define AURORA_FEBRUARY_CRIT_HPP

namespace aurora {
namespace february {

using FebruaryCritFn = void (*)(void* user);

class FebruaryCrit {
public:
    static void set(FebruaryCritFn enter, FebruaryCritFn exit,
                    void* user = nullptr) {
        enter_ = enter;
        exit_  = exit;
        user_  = user;
    }

    static void enter() {
        if (enter_) {
            enter_(user_);
        }
    }

    static void exit() {
        if (exit_) {
            exit_(user_);
        }
    }

    /** RAII guard - prefer this over manual enter/exit pairs. */
    struct Guard {
        Guard()  { FebruaryCrit::enter(); }
        ~Guard() { FebruaryCrit::exit(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };

private:
    static inline FebruaryCritFn enter_ = nullptr;
    static inline FebruaryCritFn exit_  = nullptr;
    static inline void*          user_  = nullptr;
};

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_CRIT_HPP
