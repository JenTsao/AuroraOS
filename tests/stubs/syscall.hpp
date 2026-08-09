#ifndef SYSCALL_HPP
#define SYSCALL_HPP

// =============================================================================
// syscall.hpp — Host-native STUB (overrides syscall/syscall.hpp in tests)
//
// The real syscall.hpp uses ARM SVC inline assembly (svc #N) which cannot
// compile on x86.  This stub replaces the entire file with empty no-op
// inline functions so that kernel/mutex.hpp and other consumers compile
// cleanly on the host.
// =============================================================================

#include <cstdint>

// Syscall numbers kept as constants (used by some consumers as enum-like defs)
inline constexpr uint8_t SYS_PRINT = 0x01;
inline constexpr uint8_t SYS_YIELD = 0x02;
inline constexpr uint8_t SYS_SLEEP = 0x03;

/// Print string via syscall — no-op on host.
inline void sys_print(const char* /*str*/) noexcept {}

/// Yield the CPU via syscall — no-op on host.
inline void sys_yield() noexcept {}

/// Sleep for ticks via syscall — no-op on host.
inline void sys_sleep(uint32_t /*ticks*/) noexcept {}

#endif  // SYSCALL_HPP
