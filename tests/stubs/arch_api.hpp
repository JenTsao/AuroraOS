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

inline void enable_interrupts() noexcept {
    static bool in_hook = false;
    if (g_arch_test_interrupt_hook && !in_hook) {
        in_hook = true;
        g_arch_test_interrupt_hook();
        in_hook = false;
    }
}

inline uint32_t irq_save() noexcept {
    return 0;
}

inline void irq_restore(uint32_t /*flags*/) noexcept {
    static bool in_hook = false;
    if (g_arch_test_interrupt_hook && !in_hook) {
        in_hook = true;
        g_arch_test_interrupt_hook();
        in_hook = false;
    }
}

inline void wait_for_interrupt() noexcept {}

inline void systick_init(uint32_t /*hz*/) noexcept {}

extern "C" __attribute__((weak)) void arch_test_context_switch_hook();

void host_trigger_context_switch();

inline void trigger_context_switch() noexcept {
    host_trigger_context_switch();
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

inline uint32_t* init_thread_stack(void (* /*entry*/)(void), uint32_t* stack_space, uint32_t stack_size) noexcept {
    // Return the logical stack top (high address; stack grows downward).
    return stack_space + stack_size / sizeof(uint32_t);
}

[[noreturn]] inline void start_first_task(uint32_t* /*stack_ptr*/, void (* /*entry*/)(void), uint32_t /*privilege*/) {
    throw std::logic_error("Arch::start_first_task must not be called from host unit tests");
}

inline void set_privilege(uint32_t /*privilege*/) noexcept {}

inline void switch_address_space(uintptr_t /*pgdir_base*/) noexcept {}

struct MpuRegion {
    uintptr_t base;
    uint8_t size_pow2;
    uint32_t ap;
    bool execute_never;
    bool is_device;
    uint8_t subregion_disable_mask{0};
};

inline void mpu_configure_region(uint8_t /*idx*/, const MpuRegion& /*region*/) noexcept {}

inline void mpu_enable() noexcept {}

inline void mpu_disable() noexcept {}

// ── 硬件位图加速与前导零/末尾零计算 ───────────────────────────────
inline uint32_t clz(uint32_t val) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return val ? static_cast<uint32_t>(__builtin_clz(val)) : 32u;
#elif defined(_MSC_VER)
    unsigned long index;
    return _BitScanReverse(&index, val) ? (31u - index) : 32u;
#else
    if (val == 0) return 32u;
    uint32_t n = 0;
    if ((val & 0xFFFF0000u) == 0) { n += 16; val <<= 16; }
    if ((val & 0xFF000000u) == 0) { n += 8;  val <<= 8;  }
    if ((val & 0xF0000000u) == 0) { n += 4;  val <<= 4;  }
    if ((val & 0xC0000000u) == 0) { n += 2;  val <<= 2;  }
    if ((val & 0x80000000u) == 0) { n += 1; }
    return n;
#endif
}

inline uint32_t ctz(uint32_t val) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return val ? static_cast<uint32_t>(__builtin_ctz(val)) : 32u;
#elif defined(_MSC_VER)
    unsigned long index;
    return _BitScanForward(&index, val) ? index : 32u;
#else
    if (val == 0) return 32u;
    uint32_t n = 0;
    if ((val & 0x0000FFFFu) == 0) { n += 16; val >>= 16; }
    if ((val & 0x000000FFu) == 0) { n += 8;  val >>= 8;  }
    if ((val & 0x0000000Fu) == 0) { n += 4;  val >>= 4;  }
    if ((val & 0x00000003u) == 0) { n += 2;  val >>= 2;  }
    if ((val & 0x00000001u) == 0) { n += 1; }
    return n;
#endif
}

inline int32_t find_highest_bit(uint32_t bitmask) noexcept {
    if (bitmask == 0) return -1;
#if defined(__GNUC__) || defined(__clang__)
    return 31 - __builtin_clz(bitmask);
#elif defined(_MSC_VER)
    unsigned long index;
    return _BitScanReverse(&index, bitmask) ? static_cast<int32_t>(index) : -1;
#else
    return 31 - static_cast<int32_t>(clz(bitmask));
#endif
}

inline int32_t find_lowest_bit(uint32_t bitmask) noexcept {
    if (bitmask == 0) return -1;
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctz(bitmask);
#elif defined(_MSC_VER)
    unsigned long index;
    return _BitScanForward(&index, bitmask) ? static_cast<int32_t>(index) : -1;
#else
    return static_cast<int32_t>(ctz(bitmask));
#endif
}

} // namespace Arch

#endif // ARCH_API_HPP
