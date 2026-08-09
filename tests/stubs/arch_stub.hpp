#ifndef AURORA_TESTS_ARCH_STUB_HPP
#define AURORA_TESTS_ARCH_STUB_HPP

// =============================================================================
// arch_stub.hpp — Host-native stub for Arch:: HAL interface
//
// Purpose: Allows kernel/task.hpp and kernel/frame_scheduler.hpp to compile
//          and run on x86 Linux (CI / developer workstation) without any
//          ARM Cortex-M4 toolchain or hardware.
//
// Rules:
//  - Every stub must be a no-op or return a safe sentinel.
//  - init_thread_stack() returns the top of the supplied stack so that
//    create_task() stores a non-null stack_ptr.
//  - start_first_task() is [[noreturn]] on real hardware; here we throw so
//    tests can catch it rather than hang.
// =============================================================================

#include <cstdint>
#include <stdexcept>

// Forward-declared by task.hpp via "arch_api.hpp" — we shadow that file with
// this stub by placing tests/stubs/ earlier in the include path.

namespace Arch {

/// Disable interrupts — no-op on host.
inline void disable_interrupts() noexcept {}

/// Enable interrupts — no-op on host.
inline void enable_interrupts() noexcept {}

/// Trigger PendSV context switch — no-op on host.
inline void trigger_context_switch() noexcept {}

/// Initialise a Cortex-M4 fake stack frame.
/// Returns a pointer to the top of the stack (safe sentinel for tests).
inline uint32_t* init_thread_stack(void (*/*entry*/)(void),
                                   uint32_t* stack_space,
                                   uint32_t  stack_size) noexcept {
    // Return the logical top of the stack (high address, stack grows down).
    return stack_space + stack_size / sizeof(uint32_t);
}

/// Configure SysTick — no-op on host.
inline void systick_init(uint32_t /*hz*/) noexcept {}

/// Start the first task — on host, throw so unit tests can verify startup
/// code paths without actually looping forever.
[[noreturn]] inline void start_first_task(uint32_t* /*stack_ptr*/,
                                          void (*/*entry*/)(void)) {
    throw std::logic_error("Arch::start_first_task called in host test context");
}

}  // namespace Arch

// =============================================================================
// frame_scheduler_is_task_allowed — C linkage stub
// Required by kernel/task.hpp (extern "C" declaration).
// Always returns true so schedule() never filters out tasks in host tests.
// =============================================================================
extern "C" {
inline bool frame_scheduler_is_task_allowed(uint8_t /*priority*/) noexcept {
    return true;
}
}

#endif  // AURORA_TESTS_ARCH_STUB_HPP
