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

inline void sys_cap_copy(uint32_t /*src_slot*/, uint32_t /*dst_slot*/, uint32_t /*new_rights*/) noexcept {}
inline void sys_cap_delete(uint32_t /*slot*/) noexcept {}
inline void sys_cap_mint(uint32_t /*src_slot*/, uint32_t /*dst_slot*/, uint32_t /*new_rights*/, uint32_t /*badge*/) noexcept {}

inline void sys_ipc_call(uint32_t /*cap_id*/, void* /*msg*/, uint32_t /*len*/, void* /*reply_buf*/, uint32_t /*max_reply_len*/) noexcept {}
inline void sys_ipc_receive(uint32_t /*cap_id*/, void* /*msg_buf*/, uint32_t /*max_len*/, uint32_t* /*out_sender_id*/) noexcept {}
inline void sys_ipc_reply(uint32_t /*sender_id*/, void* /*reply_msg*/, uint32_t /*len*/) noexcept {}

#endif  // SYSCALL_HPP
