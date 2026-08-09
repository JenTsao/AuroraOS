#ifndef ARCH_API_HPP
#define ARCH_API_HPP

// =============================================================================
// arch_api.hpp — Host-native STUB (overrides kernel/arch_api.hpp in tests)
//
// Placed in tests/stubs/ and injected via:
//   target_include_directories(aurora_tests BEFORE PRIVATE tests/stubs)
//
// The real kernel/arch_api.hpp pulls in arch_impl.hpp (ARM inline asm).
// This stub replaces the entire file with safe no-op implementations so that
// kernel/task.hpp, kernel/memory.hpp etc. compile on x86 without error.
// =============================================================================

#include <cstdint>
#include <stdexcept>

namespace Arch {

inline void disable_interrupts() noexcept {}
extern void (*g_arch_test_interrupt_hook)();
inline void enable_interrupts()  noexcept {
    if (g_arch_test_interrupt_hook) g_arch_test_interrupt_hook();
}
inline uint32_t irq_save()       noexcept { return 0; }
inline void irq_restore(uint32_t /*flags*/) noexcept {}
inline void wait_for_interrupt() noexcept {}

inline void systick_init(uint32_t /*hz*/) noexcept {}

extern "C" __attribute__((weak)) void arch_test_context_switch_hook();

inline void trigger_context_switch() noexcept {
    if (arch_test_context_switch_hook) {
        arch_test_context_switch_hook();
    }
}

inline uint32_t get_cycle() noexcept {
    static uint32_t simulated_cycles = 0;
    return simulated_cycles += 100;
}

inline uint32_t get_cycles_per_us() noexcept {
    return 12; // Simulate 12MHz CPU
}

inline uint32_t* init_thread_stack(void (*/*entry*/)(void),
                                   uint32_t* stack_space,
                                   uint32_t  stack_size) noexcept {
    // Return the logical stack top (high address; stack grows downward).
    return stack_space + stack_size / sizeof(uint32_t);
}

[[noreturn]] inline void start_first_task(uint32_t* /*stack_ptr*/,
                                          void (*/*entry*/)(void)) {
    throw std::logic_error(
        "Arch::start_first_task must not be called from host unit tests");
}

}  // namespace Arch

#endif  // ARCH_API_HPP
