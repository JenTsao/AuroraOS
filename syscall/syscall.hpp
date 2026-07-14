#ifndef SYSCALL_HPP
#define SYSCALL_HPP

#include <stdint.h>

constexpr uint8_t SYS_PRINT = 0x01;
constexpr uint8_t SYS_YIELD = 0x02;
constexpr uint8_t SYS_SLEEP = 0x03;
constexpr uint8_t SYS_GET_TIME = 0x04; // Reserved for time/tick getter
constexpr uint8_t SYS_EXIT     = 0x05; // Reserved for task exit/lifecycle

// Capability management (Reserved 0x08 - 0x0F)
constexpr uint8_t SYS_CAP_MINT   = 0x08;
constexpr uint8_t SYS_CAP_DERIVE = 0x09;
constexpr uint8_t SYS_CAP_REVOKE = 0x0A;
constexpr uint8_t SYS_CAP_DELETE = 0x0B;

// 微内核 IPC 通信接口
constexpr uint8_t SYS_IPC_CALL    = 0x10;
constexpr uint8_t SYS_IPC_RECEIVE = 0x11;
constexpr uint8_t SYS_IPC_REPLY   = 0x12;
constexpr uint8_t SYS_IPC_NOTIFY  = 0x13; // Reserved for async notify/signal

// POSIX 信号子系统
constexpr uint8_t SYS_KILL        = 0x14;
constexpr uint8_t SYS_SIGACTION   = 0x15;
constexpr uint8_t SYS_SIGPROCMASK = 0x16;

// 定义用户态接口
inline void sys_print(const char* str) {
#if defined(ARCH_RISCV32)
    __asm__ volatile (
        "mv a0, %0\n\t"
        "li a7, %1\n\t"
        "ecall\n\t"
        : 
        : "r"(str), "i"(SYS_PRINT)
        : "a0", "a7", "memory"
    );
#else
    __asm__ volatile (
        "mov r0, %0\n\t"
        "svc %1\n\t"
        : 
        : "r"(str), "i"(SYS_PRINT)
        : "r0", "memory"
    );
#endif
}

inline void sys_yield() {
#if defined(ARCH_RISCV32)
    __asm__ volatile (
        "li a7, %0\n\t"
        "ecall\n\t"
        :
        : "i"(SYS_YIELD)
        : "a7", "memory"
    );
#else
    __asm__ volatile (
        "svc %0"
        :
        : "i"(SYS_YIELD)
    );
#endif
}

inline void sys_sleep(uint32_t ticks) {
#if defined(ARCH_RISCV32)
    __asm__ volatile (
        "mv a0, %0\n\t"
        "li a7, %1\n\t"
        "ecall\n\t"
        : 
        : "r"(ticks), "i"(SYS_SLEEP)
        : "a0", "a7", "memory"
    );
#else
    __asm__ volatile (
        "mov r0, %0\n\t"
        "svc %1\n\t"
        : 
        : "r"(ticks), "i"(SYS_SLEEP)
        : "r0", "memory"
    );
#endif
}

inline void sys_cap_copy(uint32_t src_slot, uint32_t dst_slot, uint32_t new_rights) {
#if defined(ARCH_RISCV32)
    __asm__ volatile (
        "mv a0, %0\n\t"
        "mv a1, %1\n\t"
        "mv a2, %2\n\t"
        "li a7, %3\n\t"
        "ecall\n\t"
        : 
        : "r"(src_slot), "r"(dst_slot), "r"(new_rights), "i"(SYS_CAP_DERIVE)
        : "a0", "a1", "a2", "a7", "memory"
    );
#else
    __asm__ volatile (
        "mov r0, %0\n\t"
        "mov r1, %1\n\t"
        "mov r2, %2\n\t"
        "svc %3\n\t"
        : 
        : "r"(src_slot), "r"(dst_slot), "r"(new_rights), "i"(SYS_CAP_DERIVE)
        : "r0", "r1", "r2", "memory"
    );
#endif
}

inline void sys_cap_delete(uint32_t slot) {
#if defined(ARCH_RISCV32)
    __asm__ volatile (
        "mv a0, %0\n\t"
        "li a7, %1\n\t"
        "ecall\n\t"
        : 
        : "r"(slot), "i"(SYS_CAP_DELETE)
        : "a0", "a7", "memory"
    );
#else
    __asm__ volatile (
        "mov r0, %0\n\t"
        "svc %1\n\t"
        : 
        : "r"(slot), "i"(SYS_CAP_DELETE)
        : "r0", "memory"
    );
#endif
}

inline void sys_cap_mint(uint32_t src_slot, uint32_t dst_slot, uint32_t new_rights, uint32_t badge) {
#if defined(ARCH_RISCV32)
    __asm__ volatile (
        "mv a0, %0\n\t"
        "mv a1, %1\n\t"
        "mv a2, %2\n\t"
        "mv a3, %3\n\t"
        "li a7, %4\n\t"
        "ecall\n\t"
        : 
        : "r"(src_slot), "r"(dst_slot), "r"(new_rights), "r"(badge), "i"(SYS_CAP_MINT)
        : "a0", "a1", "a2", "a3", "a7", "memory"
    );
#else
    __asm__ volatile (
        "mov r0, %0\n\t"
        "mov r1, %1\n\t"
        "mov r2, %2\n\t"
        "mov r3, %3\n\t"
        "svc %4\n\t"
        : 
        : "r"(src_slot), "r"(dst_slot), "r"(new_rights), "r"(badge), "i"(SYS_CAP_MINT)
        : "r0", "r1", "r2", "r3", "memory"
    );
#endif
}

inline int sys_kill(uint32_t target_id, int sig) {
    int ret;
#if defined(ARCH_RISCV32)
    __asm__ volatile (
        "mv a0, %1\n\t"
        "mv a1, %2\n\t"
        "li a7, %3\n\t"
        "ecall\n\t"
        "mv %0, a0\n\t"
        : "=r"(ret)
        : "r"(target_id), "r"(sig), "i"(SYS_KILL)
        : "a0", "a1", "a7", "memory"
    );
#else
    __asm__ volatile (
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "svc %3\n\t"
        "mov %0, r0\n\t"
        : "=r"(ret)
        : "r"(target_id), "r"(sig), "i"(SYS_KILL)
        : "r0", "r1", "memory"
    );
#endif
    return ret;
}

inline int sys_sigaction(int sig, const void* act, void* oldact) {
    int ret;
#if defined(ARCH_RISCV32)
    __asm__ volatile (
        "mv a0, %1\n\t"
        "mv a1, %2\n\t"
        "mv a2, %3\n\t"
        "li a7, %4\n\t"
        "ecall\n\t"
        "mv %0, a0\n\t"
        : "=r"(ret)
        : "r"(sig), "r"(act), "r"(oldact), "i"(SYS_SIGACTION)
        : "a0", "a1", "a2", "a7", "memory"
    );
#else
    __asm__ volatile (
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "mov r2, %3\n\t"
        "svc %4\n\t"
        "mov %0, r0\n\t"
        : "=r"(ret)
        : "r"(sig), "r"(act), "r"(oldact), "i"(SYS_SIGACTION)
        : "r0", "r1", "r2", "memory"
    );
#endif
    return ret;
}

inline int sys_sigprocmask(int how, const uint32_t* set, uint32_t* oldset) {
    int ret;
#if defined(ARCH_RISCV32)
    __asm__ volatile (
        "mv a0, %1\n\t"
        "mv a1, %2\n\t"
        "mv a2, %3\n\t"
        "li a7, %4\n\t"
        "ecall\n\t"
        "mv %0, a0\n\t"
        : "=r"(ret)
        : "r"(how), "r"(set), "r"(oldset), "i"(SYS_SIGPROCMASK)
        : "a0", "a1", "a2", "a7", "memory"
    );
#else
    __asm__ volatile (
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "mov r2, %3\n\t"
        "svc %4\n\t"
        "mov %0, r0\n\t"
        : "=r"(ret)
        : "r"(how), "r"(set), "r"(oldset), "i"(SYS_SIGPROCMASK)
        : "r0", "r1", "r2", "memory"
    );
#endif
    return ret;
}

// IPC Reply Buffer descriptor to overcome 4-register limit in SVC frame
struct IpcReplyDesc {
    void* buf;
    uint32_t max_len;
};

// IPC: 发送并阻塞等待回复 (同步机制)
inline void sys_ipc_call(uint32_t cap_id, void* msg, uint32_t len, void* reply_buf, uint32_t max_reply_len) {
    IpcReplyDesc desc = { reply_buf, max_reply_len };
#if defined(ARCH_RISCV32)
    __asm__ volatile (
        "mv a0, %0\n\t"
        "mv a1, %1\n\t"
        "mv a2, %2\n\t"
        "mv a3, %3\n\t"
        "li a7, %4\n\t"
        "ecall\n\t"
        : 
        : "r"(cap_id), "r"(msg), "r"(len), "r"(&desc), "i"(SYS_IPC_CALL)
        : "a0", "a1", "a2", "a3", "a7", "memory"
    );
#else
    __asm__ volatile (
        "mov r0, %0\n\t"
        "mov r1, %1\n\t"
        "mov r2, %2\n\t"
        "mov r3, %3\n\t"
        "svc %4\n\t"
        : 
        : "r"(cap_id), "r"(msg), "r"(len), "r"(&desc), "i"(SYS_IPC_CALL)
        : "r0", "r1", "r2", "r3", "memory"
    );
#endif
}

// IPC: 接收请求 (阻塞)
inline void sys_ipc_receive(uint32_t cap_id, void* msg_buf, uint32_t max_len, uint32_t* out_sender_id) {
#if defined(ARCH_RISCV32)
    __asm__ volatile (
        "mv a0, %0\n\t"
        "mv a1, %1\n\t"
        "mv a2, %2\n\t"
        "mv a3, %3\n\t"
        "li a7, %4\n\t"
        "ecall\n\t"
        : 
        : "r"(cap_id), "r"(msg_buf), "r"(max_len), "r"(out_sender_id), "i"(SYS_IPC_RECEIVE)
        : "a0", "a1", "a2", "a3", "a7", "memory"
    );
#else
    __asm__ volatile (
        "mov r0, %0\n\t"
        "mov r1, %1\n\t"
        "mov r2, %2\n\t"
        "mov r3, %3\n\t"
        "svc %4\n\t"
        : 
        : "r"(cap_id), "r"(msg_buf), "r"(max_len), "r"(out_sender_id), "i"(SYS_IPC_RECEIVE)
        : "r0", "r1", "r2", "r3", "memory"
    );
#endif
}

// IPC: 回复请求 (非阻塞，对方恢复执行)
inline void sys_ipc_reply(uint32_t sender_id, void* reply_msg, uint32_t len) {
#if defined(ARCH_RISCV32)
    __asm__ volatile (
        "mv a0, %0\n\t"
        "mv a1, %1\n\t"
        "mv a2, %2\n\t"
        "li a7, %3\n\t"
        "ecall\n\t"
        : 
        : "r"(sender_id), "r"(reply_msg), "r"(len), "i"(SYS_IPC_REPLY)
        : "a0", "a1", "a2", "a7", "memory"
    );
#else
    __asm__ volatile (
        "mov r0, %0\n\t"
        "mov r1, %1\n\t"
        "mov r2, %2\n\t"
        "svc %3\n\t"
        : 
        : "r"(sender_id), "r"(reply_msg), "r"(len), "i"(SYS_IPC_REPLY)
        : "r0", "r1", "r2", "memory"
    );
#endif
}

#endif
