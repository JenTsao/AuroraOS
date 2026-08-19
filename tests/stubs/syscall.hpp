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

inline constexpr uint8_t SYS_PRINT = 0x01;
inline constexpr uint8_t SYS_YIELD = 0x02;
inline constexpr uint8_t SYS_SLEEP = 0x03;
inline constexpr uint8_t SYS_GET_TIME = 0x04; // Reserved for time/tick getter
inline constexpr uint8_t SYS_EXIT = 0x05;     // Reserved for task exit/lifecycle
inline constexpr uint8_t SYS_CAP_MINT = 0x08;
inline constexpr uint8_t SYS_CAP_DERIVE = 0x09;
inline constexpr uint8_t SYS_CAP_REVOKE = 0x0A;
inline constexpr uint8_t SYS_CAP_DELETE = 0x0B;
inline constexpr uint8_t SYS_CAP_GRANT = 0x0C;
inline constexpr uint8_t SYS_IPC_CALL = 0x10;
inline constexpr uint8_t SYS_IPC_RECEIVE = 0x11;
inline constexpr uint8_t SYS_IPC_REPLY = 0x12;
inline constexpr uint8_t SYS_IPC_NOTIFY = 0x13; // Reserved for async notify/signal
inline constexpr uint8_t SYS_KILL = 0x14;
inline constexpr uint8_t SYS_SIGACTION = 0x15;
inline constexpr uint8_t SYS_SIGPROCMASK = 0x16;

inline constexpr uint8_t SYS_TIMER_CREATE = 0x18;
inline constexpr uint8_t SYS_TIMER_START = 0x19;
inline constexpr uint8_t SYS_TIMER_STOP = 0x1A;
inline constexpr uint8_t SYS_TIMER_DELETE = 0x1B;
inline constexpr uint8_t SYS_TIMER_GET_TIME = 0x1C;

inline constexpr uint8_t SYS_DEV_OPEN = 0x20;
inline constexpr uint8_t SYS_DEV_READ = 0x21;
inline constexpr uint8_t SYS_DEV_WRITE = 0x22;
inline constexpr uint8_t SYS_DEV_IOCTL = 0x23;
inline constexpr uint8_t SYS_DEV_REGISTER = 0x24;

struct CapGrantDesc {
    uint32_t target_task_id;
    uint32_t src_slot;
    uint32_t dst_slot;
    uint32_t new_rights;
    uint32_t badge;
};

struct IpcReplyDesc {
    void* buf;
    uint32_t max_len;
    uint32_t timeout_ms;
};

struct DevOpenDesc {
    const char* name;
    uint32_t dst_slot;
    uint32_t rights;
};

struct DevIoDesc {
    uint32_t cap_slot;
    void* buf;
    uint32_t len;
    uint32_t offset;
};

struct DevIoctlDesc {
    uint32_t cap_slot;
    uint32_t request;
    void* arg;
};

#ifdef __cplusplus
extern "C" {
#endif
void sys_print(const char* str);
#ifdef __cplusplus
}
#endif

inline void sys_yield() noexcept {}

inline void sys_sleep(uint32_t ticks) noexcept {}

inline void sys_cap_copy(uint32_t src_slot, uint32_t dst_slot, uint32_t new_rights) noexcept {}

inline void sys_cap_delete(uint32_t slot) noexcept {}

inline void sys_cap_mint(uint32_t src_slot, uint32_t dst_slot, uint32_t new_rights, uint32_t badge) noexcept {}

inline void sys_cap_revoke(uint32_t slot) noexcept {}

inline void sys_cap_grant(const CapGrantDesc* desc) noexcept {}

inline int sys_ipc_call(uint32_t cap_id, void* msg, uint32_t len, void* reply_buf, uint32_t max_reply_len, uint32_t timeout_ms = 0xFFFFFFFFU) noexcept {
    (void)cap_id; (void)msg; (void)len; (void)reply_buf; (void)max_reply_len; (void)timeout_ms;
    return 0;
}

inline int sys_ipc_call_nb(uint32_t cap_id, void* msg, uint32_t len, void* reply_buf, uint32_t max_reply_len) noexcept {
    return sys_ipc_call(cap_id, msg, len, reply_buf, max_reply_len, 0);
}

inline int sys_ipc_receive(uint32_t cap_id, void* msg_buf, uint32_t max_len, uint32_t* out_sender_id) noexcept {
    (void)cap_id; (void)msg_buf; (void)max_len; (void)out_sender_id;
    return 0;
}

inline int sys_ipc_reply(uint32_t sender_id, void* reply_msg, uint32_t len) noexcept {
    (void)sender_id; (void)reply_msg; (void)len;
    return 0;
}

inline int sys_kill(uint32_t target_id, int sig) noexcept {
    return 0;
}

inline int sys_sigaction(int sig, const void* act, void* oldact) noexcept {
    return 0;
}

inline int sys_sigprocmask(int how, const uint32_t* set, uint32_t* oldset) noexcept {
    return 0;
}

inline int sys_open_device(const char* name, uint32_t dst_slot, uint32_t rights) noexcept {
    (void)name; (void)dst_slot; (void)rights;
    return 0;
}

inline int sys_device_read(uint32_t cap_slot, char* buf, uint32_t len, uint32_t offset = 0) noexcept {
    (void)cap_slot; (void)buf; (void)len; (void)offset;
    return 0;
}

inline int sys_device_write(uint32_t cap_slot, const char* buf, uint32_t len, uint32_t offset = 0) noexcept {
    (void)cap_slot; (void)buf; (void)len; (void)offset;
    return 0;
}

inline int sys_device_ioctl(uint32_t cap_slot, uint32_t request, void* arg) noexcept {
    (void)cap_slot; (void)request; (void)arg;
    return 0;
}

inline uint32_t sys_get_time() noexcept {
    return 0;
}

inline int sys_timer_create(const void* desc) noexcept {
    (void)desc;
    return 0;
}

inline int sys_timer_start(uint32_t timer_id, const void* desc) noexcept {
    (void)timer_id; (void)desc;
    return 0;
}

inline int sys_timer_stop(uint32_t timer_id) noexcept {
    (void)timer_id;
    return 0;
}

inline int sys_timer_delete(uint32_t timer_id) noexcept {
    (void)timer_id;
    return 0;
}

inline int sys_timer_get_time(uint32_t timer_id, uint32_t* out_remaining_ms) noexcept {
    (void)timer_id; (void)out_remaining_ms;
    return 0;
}

#endif // SYSCALL_HPP
