#ifndef SYSCALL_HPP
#define SYSCALL_HPP

#include <stdint.h>

constexpr uint8_t SYS_PRINT = 0x01;
constexpr uint8_t SYS_YIELD = 0x02;
constexpr uint8_t SYS_SLEEP = 0x03;
constexpr uint8_t SYS_GET_TIME = 0x04; // Reserved for time/tick getter
constexpr uint8_t SYS_EXIT = 0x05;     // Reserved for task exit/lifecycle

// Capability management (Reserved 0x08 - 0x0F)
constexpr uint8_t SYS_CAP_MINT = 0x08;
constexpr uint8_t SYS_CAP_DERIVE = 0x09;
constexpr uint8_t SYS_CAP_REVOKE = 0x0A;
constexpr uint8_t SYS_CAP_DELETE = 0x0B;
constexpr uint8_t SYS_CAP_GRANT = 0x0C;

// 微内核 IPC 通信接口
constexpr uint8_t SYS_IPC_CALL = 0x10;
constexpr uint8_t SYS_IPC_RECEIVE = 0x11;
constexpr uint8_t SYS_IPC_REPLY = 0x12;
constexpr uint8_t SYS_IPC_NOTIFY = 0x13; // Reserved for async notify/signal

// POSIX 信号子系统
constexpr uint8_t SYS_KILL = 0x14;
constexpr uint8_t SYS_SIGACTION = 0x15;
constexpr uint8_t SYS_SIGPROCMASK = 0x16;

// 进程级定时器子系统 (0x18 - 0x1C)
constexpr uint8_t SYS_TIMER_CREATE = 0x18;
constexpr uint8_t SYS_TIMER_START = 0x19;
constexpr uint8_t SYS_TIMER_STOP = 0x1A;
constexpr uint8_t SYS_TIMER_DELETE = 0x1B;
constexpr uint8_t SYS_TIMER_GET_TIME = 0x1C;

// 设备能力对象管理与调用 (0x20 - 0x24)
constexpr uint8_t SYS_DEV_OPEN = 0x20;
constexpr uint8_t SYS_DEV_READ = 0x21;
constexpr uint8_t SYS_DEV_WRITE = 0x22;
constexpr uint8_t SYS_DEV_IOCTL = 0x23;
constexpr uint8_t SYS_DEV_REGISTER = 0x24;

// 定义用户态接口
inline void sys_print(const char* str) {
#if defined(__x86_64__) || defined(__i386__) || defined(_WIN32)
    (void)str;
#elif defined(ARCH_RISCV32)
    __asm__ volatile("mv a0, %0\n\t"
                     "li a7, %1\n\t"
                     "ecall\n\t"
                     :
                     : "r"(str), "i"(SYS_PRINT)
                     : "a0", "a7", "memory");
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    __asm__ volatile("mov x0, %0\n\t"
                     "mov x8, %1\n\t"
                     "svc #0\n\t"
                     :
                     : "r"(str), "i"(SYS_PRINT)
                     : "x0", "x8", "memory");
#else
    __asm__ volatile("mov r0, %0\n\t"
                     "svc %1\n\t"
                     :
                     : "r"(str), "i"(SYS_PRINT)
                     : "r0", "memory");
#endif
}

inline void sys_yield() {
#if defined(__x86_64__) || defined(__i386__) || defined(_WIN32)
    // No-op for tests
#elif defined(ARCH_RISCV32)
    __asm__ volatile("li a7, %0\n\t"
                     "ecall\n\t"
                     :
                     : "i"(SYS_YIELD)
                     : "a7", "memory");
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    __asm__ volatile("mov x8, %0\n\t"
                     "svc #0\n\t"
                     :
                     : "i"(SYS_YIELD)
                     : "x8", "memory");
#else
    __asm__ volatile("svc %0" : : "i"(SYS_YIELD));
#endif
}

inline void sys_sleep(uint32_t ticks) {
#if defined(__x86_64__) || defined(__i386__) || defined(_WIN32)
    (void)ticks;
#elif defined(ARCH_RISCV32)
    __asm__ volatile("mv a0, %0\n\t"
                     "li a7, %1\n\t"
                     "ecall\n\t"
                     :
                     : "r"(ticks), "i"(SYS_SLEEP)
                     : "a0", "a7", "memory");
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    __asm__ volatile("mov x0, %0\n\t"
                     "mov x8, %1\n\t"
                     "svc #0\n\t"
                     :
                     : "r"(static_cast<uint64_t>(ticks)), "i"(SYS_SLEEP)
                     : "x0", "x8", "memory");
#else
    __asm__ volatile("mov r0, %0\n\t"
                     "svc %1\n\t"
                     :
                     : "r"(ticks), "i"(SYS_SLEEP)
                     : "r0", "memory");
#endif
}

inline void sys_cap_copy(uint32_t src_slot, uint32_t dst_slot, uint32_t new_rights) {
#if defined(ARCH_RISCV32)
    __asm__ volatile("mv a0, %0\n\t"
                     "mv a1, %1\n\t"
                     "mv a2, %2\n\t"
                     "li a7, %3\n\t"
                     "ecall\n\t"
                     :
                     : "r"(src_slot), "r"(dst_slot), "r"(new_rights), "i"(SYS_CAP_DERIVE)
                     : "a0", "a1", "a2", "a7", "memory");
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    __asm__ volatile("mov x0, %0\n\t"
                     "mov x1, %1\n\t"
                     "mov x2, %2\n\t"
                     "mov x8, %3\n\t"
                     "svc #0\n\t"
                     :
                     : "r"(static_cast<uint64_t>(src_slot)), "r"(static_cast<uint64_t>(dst_slot)),
                       "r"(static_cast<uint64_t>(new_rights)), "i"(SYS_CAP_DERIVE)
                     : "x0", "x1", "x2", "x8", "memory");
#else
    __asm__ volatile("mov r0, %0\n\t"
                     "mov r1, %1\n\t"
                     "mov r2, %2\n\t"
                     "svc %3\n\t"
                     :
                     : "r"(src_slot), "r"(dst_slot), "r"(new_rights), "i"(SYS_CAP_DERIVE)
                     : "r0", "r1", "r2", "memory");
#endif
}

inline void sys_cap_delete(uint32_t slot) {
#if defined(ARCH_RISCV32)
    __asm__ volatile("mv a0, %0\n\t"
                     "li a7, %1\n\t"
                     "ecall\n\t"
                     :
                     : "r"(slot), "i"(SYS_CAP_DELETE)
                     : "a0", "a7", "memory");
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    __asm__ volatile("mov x0, %0\n\t"
                     "mov x8, %1\n\t"
                     "svc #0\n\t"
                     :
                     : "r"(static_cast<uint64_t>(slot)), "i"(SYS_CAP_DELETE)
                     : "x0", "x8", "memory");
#else
    __asm__ volatile("mov r0, %0\n\t"
                     "svc %1\n\t"
                     :
                     : "r"(slot), "i"(SYS_CAP_DELETE)
                     : "r0", "memory");
#endif
}

inline void sys_cap_mint(uint32_t src_slot, uint32_t dst_slot, uint32_t new_rights, uint32_t badge) {
#if defined(ARCH_RISCV32)
    __asm__ volatile("mv a0, %0\n\t"
                     "mv a1, %1\n\t"
                     "mv a2, %2\n\t"
                     "mv a3, %3\n\t"
                     "li a7, %4\n\t"
                     "ecall\n\t"
                     :
                     : "r"(src_slot), "r"(dst_slot), "r"(new_rights), "r"(badge), "i"(SYS_CAP_MINT)
                     : "a0", "a1", "a2", "a3", "a7", "memory");
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    __asm__ volatile("mov x0, %0\n\t"
                     "mov x1, %1\n\t"
                     "mov x2, %2\n\t"
                     "mov x3, %3\n\t"
                     "mov x8, %4\n\t"
                     "svc #0\n\t"
                     :
                     : "r"(static_cast<uint64_t>(src_slot)), "r"(static_cast<uint64_t>(dst_slot)),
                       "r"(static_cast<uint64_t>(new_rights)), "r"(static_cast<uint64_t>(badge)), "i"(SYS_CAP_MINT)
                     : "x0", "x1", "x2", "x3", "x8", "memory");
#else
    __asm__ volatile("mov r0, %0\n\t"
                     "mov r1, %1\n\t"
                     "mov r2, %2\n\t"
                     "mov r3, %3\n\t"
                     "svc %4\n\t"
                     :
                     : "r"(src_slot), "r"(dst_slot), "r"(new_rights), "r"(badge), "i"(SYS_CAP_MINT)
                     : "r0", "r1", "r2", "r3", "memory");
#endif
}

inline void sys_cap_revoke(uint32_t slot) {
#if defined(ARCH_RISCV32)
    __asm__ volatile("mv a0, %0\n\t"
                     "li a7, %1\n\t"
                     "ecall\n\t"
                     :
                     : "r"(slot), "i"(SYS_CAP_REVOKE)
                     : "a0", "a7", "memory");
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    __asm__ volatile("mov x0, %0\n\t"
                     "mov x8, %1\n\t"
                     "svc #0\n\t"
                     :
                     : "r"(static_cast<uint64_t>(slot)), "i"(SYS_CAP_REVOKE)
                     : "x0", "x8", "memory");
#else
    __asm__ volatile("mov r0, %0\n\t"
                     "svc %1\n\t"
                     :
                     : "r"(slot), "i"(SYS_CAP_REVOKE)
                     : "r0", "memory");
#endif
}

struct CapGrantDesc {
    uint32_t target_task_id;
    uint32_t src_slot;
    uint32_t dst_slot;
    uint32_t new_rights;
    uint32_t badge;
};

inline void sys_cap_grant(const CapGrantDesc* desc) {
#if defined(ARCH_RISCV32)
    __asm__ volatile("mv a0, %0\n\t"
                     "li a7, %1\n\t"
                     "ecall\n\t"
                     :
                     : "r"(desc), "i"(SYS_CAP_GRANT)
                     : "a0", "a7", "memory");
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    __asm__ volatile("mov x0, %0\n\t"
                     "mov x8, %1\n\t"
                     "svc #0\n\t"
                     :
                     : "r"(desc), "i"(SYS_CAP_GRANT)
                     : "x0", "x8", "memory");
#else
    __asm__ volatile("mov r0, %0\n\t"
                     "svc %1\n\t"
                     :
                     : "r"(desc), "i"(SYS_CAP_GRANT)
                     : "r0", "memory");
#endif
}

inline int sys_kill(uint32_t target_id, int sig) {
    int ret;
#if defined(ARCH_RISCV32)
    __asm__ volatile("mv a0, %1\n\t"
                     "mv a1, %2\n\t"
                     "li a7, %3\n\t"
                     "ecall\n\t"
                     "mv %0, a0\n\t"
                     : "=r"(ret)
                     : "r"(target_id), "r"(sig), "i"(SYS_KILL)
                     : "a0", "a1", "a7", "memory");
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    __asm__ volatile("mov x0, %1\n\t"
                     "mov x1, %2\n\t"
                     "mov x8, %3\n\t"
                     "svc #0\n\t"
                     "mov %0, x0\n\t"
                     : "=r"(ret)
                     : "r"(static_cast<uint64_t>(target_id)), "r"(static_cast<int64_t>(sig)), "i"(SYS_KILL)
                     : "x0", "x1", "x8", "memory");
#else
    __asm__ volatile("mov r0, %1\n\t"
                     "mov r1, %2\n\t"
                     "svc %3\n\t"
                     "mov %0, r0\n\t"
                     : "=r"(ret)
                     : "r"(target_id), "r"(sig), "i"(SYS_KILL)
                     : "r0", "r1", "memory");
#endif
    return ret;
}

inline int sys_sigaction(int sig, const void* act, void* oldact) {
    int ret;
#if defined(ARCH_RISCV32)
    __asm__ volatile("mv a0, %1\n\t"
                     "mv a1, %2\n\t"
                     "mv a2, %3\n\t"
                     "li a7, %4\n\t"
                     "ecall\n\t"
                     "mv %0, a0\n\t"
                     : "=r"(ret)
                     : "r"(sig), "r"(act), "r"(oldact), "i"(SYS_SIGACTION)
                     : "a0", "a1", "a2", "a7", "memory");
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    __asm__ volatile("mov x0, %1\n\t"
                     "mov x1, %2\n\t"
                     "mov x2, %3\n\t"
                     "mov x8, %4\n\t"
                     "svc #0\n\t"
                     "mov %0, x0\n\t"
                     : "=r"(ret)
                     : "r"(static_cast<int64_t>(sig)), "r"(act), "r"(oldact), "i"(SYS_SIGACTION)
                     : "x0", "x1", "x2", "x8", "memory");
#else
    __asm__ volatile("mov r0, %1\n\t"
                     "mov r1, %2\n\t"
                     "mov r2, %3\n\t"
                     "svc %4\n\t"
                     "mov %0, r0\n\t"
                     : "=r"(ret)
                     : "r"(sig), "r"(act), "r"(oldact), "i"(SYS_SIGACTION)
                     : "r0", "r1", "r2", "memory");
#endif
    return ret;
}

inline int sys_sigprocmask(int how, const uint32_t* set, uint32_t* oldset) {
    int ret;
#if defined(ARCH_RISCV32)
    __asm__ volatile("mv a0, %1\n\t"
                     "mv a1, %2\n\t"
                     "mv a2, %3\n\t"
                     "li a7, %4\n\t"
                     "ecall\n\t"
                     "mv %0, a0\n\t"
                     : "=r"(ret)
                     : "r"(how), "r"(set), "r"(oldset), "i"(SYS_SIGPROCMASK)
                     : "a0", "a1", "a2", "a7", "memory");
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    __asm__ volatile("mov x0, %1\n\t"
                     "mov x1, %2\n\t"
                     "mov x2, %3\n\t"
                     "mov x8, %4\n\t"
                     "svc #0\n\t"
                     "mov %0, x0\n\t"
                     : "=r"(ret)
                     : "r"(static_cast<int64_t>(how)), "r"(set), "r"(oldset), "i"(SYS_SIGPROCMASK)
                     : "x0", "x1", "x2", "x8", "memory");
#else
    __asm__ volatile("mov r0, %1\n\t"
                     "mov r1, %2\n\t"
                     "mov r2, %3\n\t"
                     "svc %4\n\t"
                     "mov %0, r0\n\t"
                     : "=r"(ret)
                     : "r"(how), "r"(set), "r"(oldset), "i"(SYS_SIGPROCMASK)
                     : "r0", "r1", "r2", "memory");
#endif
    return ret;
}

// IPC Reply Buffer descriptor to overcome 4-register limit in SVC frame
struct IpcReplyDesc {
    void* buf;
    uint32_t max_len;
    uint32_t timeout_ms; // 0 = non-blocking (IPC_NONBLOCK), 0xFFFFFFFF = infinite, or timeout in ms
};

// IPC: 发送并等待回复 (支持阻塞、超时与非阻塞)
inline int sys_ipc_call(uint32_t cap_id, void* msg, uint32_t len, void* reply_buf, uint32_t max_reply_len, uint32_t timeout_ms = 0xFFFFFFFFU) {
    IpcReplyDesc desc = {reply_buf, max_reply_len, timeout_ms};
    int ret = 0;
#if defined(__x86_64__) || defined(__i386__) || defined(_WIN32)
    (void)cap_id;
    (void)msg;
    (void)len;
    (void)desc;
#elif defined(ARCH_RISCV32)
    __asm__ volatile("mv a0, %1\n\t"
                     "mv a1, %2\n\t"
                     "mv a2, %3\n\t"
                     "mv a3, %4\n\t"
                     "li a7, %5\n\t"
                     "ecall\n\t"
                     "mv %0, a0\n\t"
                     : "=r"(ret)
                     : "r"(cap_id), "r"(msg), "r"(len), "r"(&desc), "i"(SYS_IPC_CALL)
                     : "a0", "a1", "a2", "a3", "a7", "memory");
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    __asm__ volatile("mov x0, %1\n\t"
                     "mov x1, %2\n\t"
                     "mov x2, %3\n\t"
                     "mov x3, %4\n\t"
                     "mov x8, %5\n\t"
                     "svc #0\n\t"
                     "mov %0, x0\n\t"
                     : "=r"(ret)
                     : "r"(static_cast<uint64_t>(cap_id)), "r"(msg), "r"(static_cast<uint64_t>(len)), "r"(&desc), "i"(SYS_IPC_CALL)
                     : "x0", "x1", "x2", "x3", "x8", "memory");
#else
    __asm__ volatile("mov r0, %1\n\t"
                     "mov r1, %2\n\t"
                     "mov r2, %3\n\t"
                     "mov r3, %4\n\t"
                     "svc %5\n\t"
                     "mov %0, r0\n\t"
                     : "=r"(ret)
                     : "r"(cap_id), "r"(msg), "r"(len), "r"(&desc), "i"(SYS_IPC_CALL)
                     : "r0", "r1", "r2", "r3", "memory");
#endif
    return ret;
}

// IPC: 非阻塞发送 (nb_call)
inline int sys_ipc_call_nb(uint32_t cap_id, void* msg, uint32_t len, void* reply_buf, uint32_t max_reply_len) {
    return sys_ipc_call(cap_id, msg, len, reply_buf, max_reply_len, 0);
}

// IPC: 接收请求 (阻塞)
inline void sys_ipc_receive(uint32_t cap_id, void* msg_buf, uint32_t max_len, uint32_t* out_sender_id) {
#if defined(__x86_64__) || defined(__i386__) || defined(_WIN32)
    (void)cap_id;
    (void)msg_buf;
    (void)max_len;
    (void)out_sender_id;
#elif defined(ARCH_RISCV32)
    __asm__ volatile("mv a0, %0\n\t"
                     "mv a1, %1\n\t"
                     "mv a2, %2\n\t"
                     "mv a3, %3\n\t"
                     "li a7, %4\n\t"
                     "ecall\n\t"
                     :
                     : "r"(cap_id), "r"(msg_buf), "r"(max_len), "r"(out_sender_id), "i"(SYS_IPC_RECEIVE)
                     : "a0", "a1", "a2", "a3", "a7", "memory");
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    __asm__ volatile("mov x0, %0\n\t"
                     "mov x1, %1\n\t"
                     "mov x2, %2\n\t"
                     "mov x3, %3\n\t"
                     "mov x8, %4\n\t"
                     "svc #0\n\t"
                     :
                     : "r"(static_cast<uint64_t>(cap_id)), "r"(msg_buf), "r"(static_cast<uint64_t>(max_len)), "r"(out_sender_id), "i"(SYS_IPC_RECEIVE)
                     : "x0", "x1", "x2", "x3", "x8", "memory");
#else
    __asm__ volatile("mov r0, %0\n\t"
                     "mov r1, %1\n\t"
                     "mov r2, %2\n\t"
                     "mov r3, %3\n\t"
                     "svc %4\n\t"
                     :
                     : "r"(cap_id), "r"(msg_buf), "r"(max_len), "r"(out_sender_id), "i"(SYS_IPC_RECEIVE)
                     : "r0", "r1", "r2", "r3", "memory");
#endif
}

// IPC: 回复请求 (非阻塞，对方恢复执行)
inline void sys_ipc_reply(uint32_t sender_id, void* reply_msg, uint32_t len) {
#if defined(__x86_64__) || defined(__i386__) || defined(_WIN32)
    (void)sender_id;
    (void)reply_msg;
    (void)len;
#elif defined(ARCH_RISCV32)
    __asm__ volatile("mv a0, %0\n\t"
                     "mv a1, %1\n\t"
                     "mv a2, %2\n\t"
                     "li a7, %3\n\t"
                     "ecall\n\t"
                     :
                     : "r"(sender_id), "r"(reply_msg), "r"(len), "i"(SYS_IPC_REPLY)
                     : "a0", "a1", "a2", "a7", "memory");
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    __asm__ volatile("mov x0, %0\n\t"
                     "mov x1, %1\n\t"
                     "mov x2, %2\n\t"
                     "mov x8, %3\n\t"
                     "svc #0\n\t"
                     :
                     : "r"(static_cast<uint64_t>(sender_id)), "r"(reply_msg), "r"(static_cast<uint64_t>(len)), "i"(SYS_IPC_REPLY)
                     : "x0", "x1", "x2", "x8", "memory");
#else
    __asm__ volatile("mov r0, %0\n\t"
                     "mov r1, %1\n\t"
                     "mov r2, %2\n\t"
                     "svc %3\n\t"
                     :
                     : "r"(sender_id), "r"(reply_msg), "r"(len), "i"(SYS_IPC_REPLY)
                     : "r0", "r1", "r2", "memory");
#endif
}

// 设备能力系统调用描述体
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

inline int sys_open_device(const char* name, uint32_t dst_slot, uint32_t rights) {
    DevOpenDesc desc = {name, dst_slot, rights};
#if defined(__x86_64__) || defined(__i386__) || defined(_WIN32)
    (void)desc;
    return 0;
#elif defined(ARCH_RISCV32)
    int ret;
    __asm__ volatile("mv a0, %1\n\t"
                     "li a7, %2\n\t"
                     "ecall\n\t"
                     "mv %0, a0\n\t"
                     : "=r"(ret)
                     : "r"(&desc), "i"(SYS_DEV_OPEN)
                     : "a0", "a7", "memory");
    return ret;
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    int ret;
    __asm__ volatile("mov x0, %1\n\t"
                     "mov x8, %2\n\t"
                     "svc #0\n\t"
                     "mov %0, x0\n\t"
                     : "=r"(ret)
                     : "r"(&desc), "i"(SYS_DEV_OPEN)
                     : "x0", "x8", "memory");
    return ret;
#else
    int ret;
    __asm__ volatile("mov r0, %1\n\t"
                     "svc %2\n\t"
                     "mov %0, r0\n\t"
                     : "=r"(ret)
                     : "r"(&desc), "i"(SYS_DEV_OPEN)
                     : "r0", "memory");
    return ret;
#endif
}

inline int sys_device_read(uint32_t cap_slot, char* buf, uint32_t len, uint32_t offset = 0) {
    DevIoDesc desc = {cap_slot, buf, len, offset};
#if defined(__x86_64__) || defined(__i386__) || defined(_WIN32)
    (void)desc;
    return 0;
#elif defined(ARCH_RISCV32)
    int ret;
    __asm__ volatile("mv a0, %1\n\t"
                     "li a7, %2\n\t"
                     "ecall\n\t"
                     "mv %0, a0\n\t"
                     : "=r"(ret)
                     : "r"(&desc), "i"(SYS_DEV_READ)
                     : "a0", "a7", "memory");
    return ret;
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    int ret;
    __asm__ volatile("mov x0, %1\n\t"
                     "mov x8, %2\n\t"
                     "svc #0\n\t"
                     "mov %0, x0\n\t"
                     : "=r"(ret)
                     : "r"(&desc), "i"(SYS_DEV_READ)
                     : "x0", "x8", "memory");
    return ret;
#else
    int ret;
    __asm__ volatile("mov r0, %1\n\t"
                     "svc %2\n\t"
                     "mov %0, r0\n\t"
                     : "=r"(ret)
                     : "r"(&desc), "i"(SYS_DEV_READ)
                     : "r0", "memory");
    return ret;
#endif
}

inline int sys_device_write(uint32_t cap_slot, const char* buf, uint32_t len, uint32_t offset = 0) {
    DevIoDesc desc = {cap_slot, const_cast<char*>(buf), len, offset};
#if defined(__x86_64__) || defined(__i386__) || defined(_WIN32)
    (void)desc;
    return 0;
#elif defined(ARCH_RISCV32)
    int ret;
    __asm__ volatile("mv a0, %1\n\t"
                     "li a7, %2\n\t"
                     "ecall\n\t"
                     "mv %0, a0\n\t"
                     : "=r"(ret)
                     : "r"(&desc), "i"(SYS_DEV_WRITE)
                     : "a0", "a7", "memory");
    return ret;
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    int ret;
    __asm__ volatile("mov x0, %1\n\t"
                     "mov x8, %2\n\t"
                     "svc #0\n\t"
                     "mov %0, x0\n\t"
                     : "=r"(ret)
                     : "r"(&desc), "i"(SYS_DEV_WRITE)
                     : "x0", "x8", "memory");
    return ret;
#else
    int ret;
    __asm__ volatile("mov r0, %1\n\t"
                     "svc %2\n\t"
                     "mov %0, r0\n\t"
                     : "=r"(ret)
                     : "r"(&desc), "i"(SYS_DEV_WRITE)
                     : "r0", "memory");
    return ret;
#endif
}

inline int sys_device_ioctl(uint32_t cap_slot, uint32_t request, void* arg) {
    DevIoctlDesc desc = {cap_slot, request, arg};
#if defined(__x86_64__) || defined(__i386__) || defined(_WIN32)
    (void)desc;
    return 0;
#elif defined(ARCH_RISCV32)
    int ret;
    __asm__ volatile("mv a0, %1\n\t"
                     "li a7, %2\n\t"
                     "ecall\n\t"
                     "mv %0, a0\n\t"
                     : "=r"(ret)
                     : "r"(&desc), "i"(SYS_DEV_IOCTL)
                     : "a0", "a7", "memory");
    return ret;
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    int ret;
    __asm__ volatile("mov x0, %1\n\t"
                     "mov x8, %2\n\t"
                     "svc #0\n\t"
                     "mov %0, x0\n\t"
                     : "=r"(ret)
                     : "r"(&desc), "i"(SYS_DEV_IOCTL)
                     : "x0", "x8", "memory");
    return ret;
#else
    int ret;
    __asm__ volatile("mov r0, %1\n\t"
                     "svc %2\n\t"
                     "mov %0, r0\n\t"
                     : "=r"(ret)
                     : "r"(&desc), "i"(SYS_DEV_IOCTL)
                     : "r0", "memory");
    return ret;
#endif
}

inline uint32_t sys_get_time() {
#if defined(__x86_64__) || defined(__i386__) || defined(_WIN32)
    return 0;
#elif defined(ARCH_RISCV32)
    uint32_t ret;
    __asm__ volatile("li a7, %1\n\t"
                     "ecall\n\t"
                     "mv %0, a0\n\t"
                     : "=r"(ret)
                     : "i"(SYS_GET_TIME)
                     : "a0", "a7", "memory");
    return ret;
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    uint32_t ret;
    __asm__ volatile("mov x8, %1\n\t"
                     "svc #0\n\t"
                     "mov %0, w0\n\t"
                     : "=r"(ret)
                     : "i"(SYS_GET_TIME)
                     : "w0", "x8", "memory");
    return ret;
#else
    uint32_t ret;
    __asm__ volatile("svc %1\n\t"
                     "mov %0, r0\n\t"
                     : "=r"(ret)
                     : "i"(SYS_GET_TIME)
                     : "r0", "memory");
    return ret;
#endif
}

inline int sys_timer_create(const void* desc) {
#if defined(__x86_64__) || defined(__i386__) || defined(_WIN32)
    (void)desc;
    return 0;
#elif defined(ARCH_RISCV32)
    int ret;
    __asm__ volatile("mv a0, %1\n\t"
                     "li a7, %2\n\t"
                     "ecall\n\t"
                     "mv %0, a0\n\t"
                     : "=r"(ret)
                     : "r"(desc), "i"(SYS_TIMER_CREATE)
                     : "a0", "a7", "memory");
    return ret;
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    int ret;
    __asm__ volatile("mov x0, %1\n\t"
                     "mov x8, %2\n\t"
                     "svc #0\n\t"
                     "mov %0, x0\n\t"
                     : "=r"(ret)
                     : "r"(desc), "i"(SYS_TIMER_CREATE)
                     : "x0", "x8", "memory");
    return ret;
#else
    int ret;
    __asm__ volatile("mov r0, %1\n\t"
                     "svc %2\n\t"
                     "mov %0, r0\n\t"
                     : "=r"(ret)
                     : "r"(desc), "i"(SYS_TIMER_CREATE)
                     : "r0", "memory");
    return ret;
#endif
}

inline int sys_timer_start(uint32_t timer_id, const void* desc) {
#if defined(__x86_64__) || defined(__i386__) || defined(_WIN32)
    (void)timer_id;
    (void)desc;
    return 0;
#elif defined(ARCH_RISCV32)
    int ret;
    __asm__ volatile("mv a0, %1\n\t"
                     "mv a1, %2\n\t"
                     "li a7, %3\n\t"
                     "ecall\n\t"
                     "mv %0, a0\n\t"
                     : "=r"(ret)
                     : "r"(timer_id), "r"(desc), "i"(SYS_TIMER_START)
                     : "a0", "a1", "a7", "memory");
    return ret;
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    int ret;
    __asm__ volatile("mov x0, %1\n\t"
                     "mov x1, %2\n\t"
                     "mov x8, %3\n\t"
                     "svc #0\n\t"
                     "mov %0, x0\n\t"
                     : "=r"(ret)
                     : "r"(static_cast<uint64_t>(timer_id)), "r"(desc), "i"(SYS_TIMER_START)
                     : "x0", "x1", "x8", "memory");
    return ret;
#else
    int ret;
    __asm__ volatile("mov r0, %1\n\t"
                     "mov r1, %2\n\t"
                     "svc %3\n\t"
                     "mov %0, r0\n\t"
                     : "=r"(ret)
                     : "r"(timer_id), "r"(desc), "i"(SYS_TIMER_START)
                     : "r0", "r1", "memory");
    return ret;
#endif
}

inline int sys_timer_stop(uint32_t timer_id) {
#if defined(__x86_64__) || defined(__i386__) || defined(_WIN32)
    (void)timer_id;
    return 0;
#elif defined(ARCH_RISCV32)
    int ret;
    __asm__ volatile("mv a0, %1\n\t"
                     "li a7, %2\n\t"
                     "ecall\n\t"
                     "mv %0, a0\n\t"
                     : "=r"(ret)
                     : "r"(timer_id), "i"(SYS_TIMER_STOP)
                     : "a0", "a7", "memory");
    return ret;
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    int ret;
    __asm__ volatile("mov x0, %1\n\t"
                     "mov x8, %2\n\t"
                     "svc #0\n\t"
                     "mov %0, x0\n\t"
                     : "=r"(ret)
                     : "r"(static_cast<uint64_t>(timer_id)), "i"(SYS_TIMER_STOP)
                     : "x0", "x8", "memory");
    return ret;
#else
    int ret;
    __asm__ volatile("mov r0, %1\n\t"
                     "svc %2\n\t"
                     "mov %0, r0\n\t"
                     : "=r"(ret)
                     : "r"(timer_id), "i"(SYS_TIMER_STOP)
                     : "r0", "memory");
    return ret;
#endif
}

inline int sys_timer_delete(uint32_t timer_id) {
#if defined(__x86_64__) || defined(__i386__) || defined(_WIN32)
    (void)timer_id;
    return 0;
#elif defined(ARCH_RISCV32)
    int ret;
    __asm__ volatile("mv a0, %1\n\t"
                     "li a7, %2\n\t"
                     "ecall\n\t"
                     "mv %0, a0\n\t"
                     : "=r"(ret)
                     : "r"(timer_id), "i"(SYS_TIMER_DELETE)
                     : "a0", "a7", "memory");
    return ret;
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    int ret;
    __asm__ volatile("mov x0, %1\n\t"
                     "mov x8, %2\n\t"
                     "svc #0\n\t"
                     "mov %0, x0\n\t"
                     : "=r"(ret)
                     : "r"(static_cast<uint64_t>(timer_id)), "i"(SYS_TIMER_DELETE)
                     : "x0", "x8", "memory");
    return ret;
#else
    int ret;
    __asm__ volatile("mov r0, %1\n\t"
                     "svc %2\n\t"
                     "mov %0, r0\n\t"
                     : "=r"(ret)
                     : "r"(timer_id), "i"(SYS_TIMER_DELETE)
                     : "r0", "memory");
    return ret;
#endif
}

inline int sys_timer_get_time(uint32_t timer_id, uint32_t* out_remaining_ms) {
#if defined(__x86_64__) || defined(__i386__) || defined(_WIN32)
    (void)timer_id;
    (void)out_remaining_ms;
    return 0;
#elif defined(ARCH_RISCV32)
    int ret;
    __asm__ volatile("mv a0, %1\n\t"
                     "mv a1, %2\n\t"
                     "li a7, %3\n\t"
                     "ecall\n\t"
                     "mv %0, a0\n\t"
                     : "=r"(ret)
                     : "r"(timer_id), "r"(out_remaining_ms), "i"(SYS_TIMER_GET_TIME)
                     : "a0", "a1", "a7", "memory");
    return ret;
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
    int ret;
    __asm__ volatile("mov x0, %1\n\t"
                     "mov x1, %2\n\t"
                     "mov x8, %3\n\t"
                     "svc #0\n\t"
                     "mov %0, x0\n\t"
                     : "=r"(ret)
                     : "r"(static_cast<uint64_t>(timer_id)), "r"(out_remaining_ms), "i"(SYS_TIMER_GET_TIME)
                     : "x0", "x1", "x8", "memory");
    return ret;
#else
    int ret;
    __asm__ volatile("mov r0, %1\n\t"
                     "mov r1, %2\n\t"
                     "svc %3\n\t"
                     "mov %0, r0\n\t"
                     : "=r"(ret)
                     : "r"(timer_id), "r"(out_remaining_ms), "i"(SYS_TIMER_GET_TIME)
                     : "r0", "r1", "memory");
    return ret;
#endif
}

#endif
