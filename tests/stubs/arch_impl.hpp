#ifndef ARCH_IMPL_HPP
#define ARCH_IMPL_HPP

// =============================================================================
// arch_impl.hpp — Host-native STUB
//
// The real arch/arm/cortex-m/cm4/arch_impl.hpp provides Cortex-M4 inline
// assembly bodies for the Arch:: functions declared in arch_api.hpp.
// kernel/arch_api.hpp ends with:  #include "arch_impl.hpp"
//
// Important: The declarations in arch_api.hpp have no 'noexcept'.
// These inline definitions must match exactly or the compiler will error
// "different exception specifier".  So we intentionally omit noexcept here.
// =============================================================================

#include <cstdint>
#include <stdexcept>

namespace Arch {

// Match declarations in arch_api.hpp exactly (no noexcept, same signature).
extern void (*g_arch_test_interrupt_hook)();
inline void disable_interrupts()           {}
inline void enable_interrupts()            {
    if (g_arch_test_interrupt_hook) g_arch_test_interrupt_hook();
}
inline uint32_t irq_save()                 { return 0u; }
inline void irq_restore(uint32_t /*flags*/) {
    // IrqGuard destructs here — same semantics as enable_interrupts():
    // advance simulated time so that timeout-based tests make progress.
    if (g_arch_test_interrupt_hook) g_arch_test_interrupt_hook();
}
inline void wait_for_interrupt()           {}
inline void systick_init(uint32_t /*hz*/)  {}
inline void trigger_context_switch()       {}

inline uint32_t get_cycle() {
    static uint32_t simulated_cycles = 0;
    return simulated_cycles += 100;
}

inline uint32_t get_cycles_per_us() {
    return 12; // Simulate 12MHz CPU
}

inline uint32_t* init_thread_stack(void (*/*entry*/)(void),
                                   uint32_t* stack_space,
                                   uint32_t  stack_size) {
    // Return the logical stack top (high address; stack grows downward).
    return stack_space + stack_size / sizeof(uint32_t);
}

[[noreturn]] inline void start_first_task(uint32_t* /*stack_ptr*/,
                                          void (*/*entry*/)(void),
                                          uint32_t /*privilege*/) {
    throw std::logic_error(
        "Arch::start_first_task must not be called from host unit tests");
}

}  // namespace Arch

#endif  // ARCH_IMPL_HPP
