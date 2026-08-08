#ifndef AURORA_KERNEL_AUDIT_HPP
#define AURORA_KERNEL_AUDIT_HPP

#include <stdint.h>
#include <stddef.h>
#include "task.hpp"
#include "mutex.hpp"
#include "../vfs/vfs.hpp"

// ============================================================
// 系统调用审计引擎 (System Call Audit Engine)
//
// 功能：
//   1. 记录所有 SYS_OPEN / SYS_WRITE / SYS_SOCKET 等关键调用
//   2. 128 槽环形缓冲区，ISR 安全写入
//   3. ProcFS 节点 /proc/audit_log 实时查询
//   4. Lua 规则引擎匹配异常行为（路径模式、阈值告警）
//
// 集成点：
//   - posix.cpp: 在 open/read/write/close 调用时钩入
//   - interrupts.cpp: 在 SVC_Handler_C 的每个 case 钩入
//   - MiniProgramEngine: 通过 Lua 绑定注册自定义审计规则
//
// 遵循 C++ Core Guidelines: RAII, enum class, constexpr, noexcept
// ============================================================

// 审计事件类型
enum class AuditEvent : uint8_t {
    // POSIX 文件 I/O
    FileOpen   = 0,   // open()
    FileClose  = 1,   // close()
    FileRead   = 2,   // read()
    FileWrite  = 3,   // write()
    FileSeek   = 4,   // lseek()
    FileIoctl  = 5,   // ioctl()

    // 网络 Socket
    SocketCreate   = 10,  // socket()
    SocketConnect  = 11,  // connect()
    SocketSend     = 12,  // send() / sendto()
    SocketRecv     = 13,  // recv() / recvfrom()
    SocketClose    = 14,  // close() on socket

    // 系统调用 (SVC)
    SysPrint     = 20,  // SYS_PRINT
    SysYield     = 21,  // SYS_YIELD
    SysSleep     = 22,  // SYS_SLEEP
    SysCapDerive = 23,  // SYS_CAP_DERIVE
    SysCapMint   = 24,  // SYS_CAP_MINT
    SysCapRevoke = 25,  // SYS_CAP_REVOKE
    SysCapGrant  = 26,  // SYS_CAP_GRANT
    SysCapDelete = 27,  // SYS_CAP_DELETE
    SysKill      = 28,  // SYS_KILL
    SysSigAction = 29,  // SYS_SIGACTION
    SysSigMask   = 30,  // SYS_SIGPROCMASK
    SysIpcCall   = 31,  // SYS_IPC_CALL
    SysIpcRecv   = 32,  // SYS_IPC_RECEIVE
    SysIpcReply  = 33,  // SYS_IPC_REPLY
    SysUnknown   = 34,  // 未知 SVC（潜在攻击探测）

    // 审计规则告警
    AuditAlert   = 40   // 规则引擎产生的告警
};

// 审计条目（32 字节，缓存行友好，适合环形缓冲区高吞吐）
struct AuditEntry {
    uint32_t  timestamp;   // 时间戳（tick）
    uint16_t  task_id;     // 调用者任务 ID
    uint8_t   event;       // AuditEvent 类型
    uint8_t   result;      // 0=成功, 非0=失败
    uint16_t  arg0;        // fd / port / svc_number
    uint32_t  arg1;        // len / flags / ip (低 32 位)
    char      path[16];    // 截断路径（仅 FileOpen 有意义）
};

// 审计规则（供 Lua 脚本定义）
struct AuditRule {
    uint8_t   event_filter;    // 筛选事件类型（255=全部）
    char      path_pattern[32]; // 路径匹配模式（支持 * 通配符）
    uint32_t  tid_filter;      // 筛选 TID（0=全部）
    uint16_t  threshold_count; // 时间窗口内触发次数阈值
    uint32_t  threshold_ms;   // 时间窗口（毫秒）
    uint8_t   action;          // 0=仅记录, 1=告警, 2=终止任务
    bool      active;          // 规则是否启用
};

// ============================================================
// 审计日志 ProcFS 节点: /proc/audit_log
// ============================================================
class AuditLogNode : public VNode {
public:
    void set_buffer(const AuditEntry* buf, int capacity, volatile int* count) {
        buffer_    = buf;
        capacity_ = capacity;
        count_    = count;
    }

    int read(char* buf, int len, int offset, void* /*priv*/) override {
        if (!buffer_ || !count_) return 0;

        int total = *count_;
        int readable = (total > capacity_) ? capacity_ : total;
        if (offset >= readable) return 0;

        int pos = 0;
        auto app_s = [&](const char* s) {
            while (*s && pos < len - 1) buf[pos++] = *s++;
        };
        auto app_u = [&](uint32_t num) {
            char tmp[16]; int i = 0;
            if (num == 0) { tmp[i++] = '0'; }
            while (num > 0) { tmp[i++] = '0' + (num % 10); num /= 10; }
            while (i > 0 && pos < len - 1) buf[pos++] = tmp[--i];
        };

        // 从 offset 开始遍历（最新条目在前）
        int idx = readable - 1 - offset;
        while (idx >= 0 && pos < len - 80) {
            const AuditEntry& e = buffer_[idx % capacity_];

            app_u(e.timestamp);               app_s("\t");
            app_u(e.task_id);                 app_s("\t");
            app_s(event_name_(e.event));      app_s("\t");
            app_s(e.result == 0 ? "OK" : "FAIL"); app_s("\t");

            if (e.path[0]) {
                app_s(e.path);
            } else {
                app_u(e.arg0);
            }
            app_s("\t");
            app_u(e.arg1);
            app_s("\n");

            --idx;
        }

        buf[pos] = '\0';
        return pos;
    }

private:
    const AuditEntry* buffer_   = nullptr;
    int               capacity_ = 0;
    volatile int*     count_    = nullptr;

    static const char* event_name_(uint8_t ev) {
        switch (static_cast<AuditEvent>(ev)) {
            case AuditEvent::FileOpen:       return "open";
            case AuditEvent::FileClose:      return "close";
            case AuditEvent::FileRead:       return "read";
            case AuditEvent::FileWrite:      return "write";
            case AuditEvent::FileSeek:       return "lseek";
            case AuditEvent::FileIoctl:      return "ioctl";
            case AuditEvent::SocketCreate:   return "socket";
            case AuditEvent::SocketConnect:  return "connect";
            case AuditEvent::SocketSend:     return "send";
            case AuditEvent::SocketRecv:     return "recv";
            case AuditEvent::SocketClose:    return "sockclose";
            case AuditEvent::SysPrint:       return "svc_print";
            case AuditEvent::SysYield:       return "svc_yield";
            case AuditEvent::SysSleep:       return "svc_sleep";
            case AuditEvent::SysCapDerive:   return "cap_derive";
            case AuditEvent::SysCapMint:     return "cap_mint";
            case AuditEvent::SysCapRevoke:   return "cap_revoke";
            case AuditEvent::SysCapGrant:    return "cap_grant";
            case AuditEvent::SysCapDelete:   return "cap_delete";
            case AuditEvent::SysKill:        return "kill";
            case AuditEvent::SysSigAction:   return "sigaction";
            case AuditEvent::SysSigMask:     return "sigprocmask";
            case AuditEvent::SysIpcCall:     return "ipc_call";
            case AuditEvent::SysIpcRecv:     return "ipc_recv";
            case AuditEvent::SysIpcReply:    return "ipc_reply";
            case AuditEvent::SysUnknown:     return "svc_unknown";
            case AuditEvent::AuditAlert:     return "ALERT";
            default:                         return "?";
        }
    }
};

// ============================================================
// 审计引擎主类
// ============================================================
class AuditEngine {
public:
    static AuditEngine& instance() noexcept {
        static AuditEngine engine;
        return engine;
    }

    // ---- 初始化 ----
    // 必须在 VFS 初始化后调用，挂载 /proc/audit_log
    void init() {
        if (initialized_) return;

        audit_log_node_.set_buffer(ring_buffer_, kRingCapacity, &write_count_);
        VfsManager::instance().mount("/proc/audit_log", &audit_log_node_);

        initialized_ = true;
    }

    // ---- 核心记录接口 (ISR 安全，关中断写入) ----

    // 快速记录（无路径）
    void record(AuditEvent event, uint8_t result, uint16_t arg0, uint32_t arg1) {
        AuditEntry entry{};
        entry.timestamp = get_tick_();
        entry.task_id   = get_current_tid_();
        entry.event     = static_cast<uint8_t>(event);
        entry.result    = result;
        entry.arg0      = arg0;
        entry.arg1      = arg1;
        entry.path[0]   = '\0';

        push_entry_(entry);
    }

    // 带路径记录（用于 FileOpen 等；路径自动截断到 15 字符+'\0'）
    void record_path(AuditEvent event, uint8_t result, uint16_t arg0,
                     uint32_t arg1, const char* path) {
        AuditEntry entry{};
        entry.timestamp = get_tick_();
        entry.task_id   = get_current_tid_();
        entry.event     = static_cast<uint8_t>(event);
        entry.result    = result;
        entry.arg0      = arg0;
        entry.arg1      = arg1;

        if (path) {
            int i = 0;
            while (path[i] && i < 15) {
                entry.path[i] = path[i];
                ++i;
            }
            entry.path[i] = '\0';
        } else {
            entry.path[0] = '\0';
        }

        push_entry_(entry);
    }

    // 记录 SVC 调用（由 interrupts.cpp 调用）
    void record_svc(uint8_t svc_number, uint8_t result) {
        AuditEvent event = map_svc_to_event_(svc_number);
        record(event, result, svc_number, 0);
    }

    // ---- 便捷封装（供 posix.cpp 内联调用） ----

    void record_open(const char* path, int fd, int flags) {
        uint8_t result = (fd < 0) ? 1 : 0;
        record_path(AuditEvent::FileOpen, result,
                    static_cast<uint16_t>(fd >= 0 ? fd : 0),
                    static_cast<uint32_t>(flags), path);
    }

    void record_write(int fd, int bytes_written) {
        uint8_t result = (bytes_written < 0) ? 1 : 0;
        record(AuditEvent::FileWrite, result,
               static_cast<uint16_t>(fd),
               static_cast<uint32_t>(bytes_written > 0 ? bytes_written : 0));
    }

    void record_read(int fd, int bytes_read) {
        uint8_t result = (bytes_read < 0) ? 1 : 0;
        record(AuditEvent::FileRead, result,
               static_cast<uint16_t>(fd),
               static_cast<uint32_t>(bytes_read > 0 ? bytes_read : 0));
    }

    void record_close(int fd, int ret) {
        uint8_t result = (ret < 0) ? 1 : 0;
        record(AuditEvent::FileClose, result, static_cast<uint16_t>(fd), 0);
    }

    void record_socket_create(int domain, int sock_fd) {
        uint8_t result = (sock_fd < 0) ? 1 : 0;
        record(AuditEvent::SocketCreate, result,
               static_cast<uint16_t>(sock_fd >= 0 ? sock_fd : 0),
               static_cast<uint32_t>(domain));
    }

    void record_socket_connect(int sock_fd, uint32_t ip, uint16_t port) {
        record(AuditEvent::SocketConnect, 0,
               static_cast<uint16_t>(sock_fd),
               ip); // port 暂存于 arg0 高位，后续可扩展
        (void)port;
    }

    void record_socket_send(int sock_fd, int bytes_sent) {
        uint8_t result = (bytes_sent < 0) ? 1 : 0;
        record(AuditEvent::SocketSend, result,
               static_cast<uint16_t>(sock_fd),
               static_cast<uint32_t>(bytes_sent > 0 ? bytes_sent : 0));
    }

    void record_socket_recv(int sock_fd, int bytes_recv) {
        uint8_t result = (bytes_recv < 0) ? 1 : 0;
        record(AuditEvent::SocketRecv, result,
               static_cast<uint16_t>(sock_fd),
               static_cast<uint32_t>(bytes_recv > 0 ? bytes_recv : 0));
    }

    // ---- 审计规则引擎 ----

    // 注册一条审计规则（供 Lua 绑定调用）
    bool add_rule(const AuditRule& rule) {
        for (int i = 0; i < kMaxRules; ++i) {
            if (!rules_[i].active) {
                rules_[i] = rule;
                rules_[i].active = true;
                return true;
            }
        }
        return false; // 规则表已满
    }

    // 移除规则（按索引）
    void remove_rule(int index) {
        if (index >= 0 && index < kMaxRules) {
            rules_[index].active = false;
        }
    }

    // 清空所有规则
    void clear_rules() {
        for (int i = 0; i < kMaxRules; ++i) {
            rules_[i].active = false;
        }
    }

    // 评估所有规则（在每条记录写入后自动调用）
    void evaluate_rules(const AuditEntry& entry) {
        for (int i = 0; i < kMaxRules; ++i) {
            const AuditRule& rule = rules_[i];
            if (!rule.active) continue;

            // 事件类型筛选
            if (rule.event_filter != 0xFF &&
                rule.event_filter != entry.event) {
                continue;
            }

            // TID 筛选
            if (rule.tid_filter != 0 &&
                rule.tid_filter != entry.task_id) {
                continue;
            }

            // 路径模式匹配
            if (rule.path_pattern[0] != '\0' && entry.path[0] != '\0') {
                if (!path_match_(entry.path, rule.path_pattern)) {
                    continue;
                }
            }

            // 阈值检查（在时间窗口内统计匹配次数）
            if (rule.threshold_count > 0) {
                uint32_t window_start = entry.timestamp - (rule.threshold_ms * 1000 / 1000); // ms→ticks（假设1ms/tick）
                int match_count = 0;
                int total = write_count_;
                int limit = (total > kRingCapacity) ? kRingCapacity : total;
                for (int j = 0; j < limit; ++j) {
                    int idx = (total - 1 - j) % kRingCapacity;
                    if (idx < 0) idx += kRingCapacity;
                    const AuditEntry& hist = ring_buffer_[idx];

                    if (hist.timestamp < window_start) break; // 超出窗口

                    if (rule.event_filter == 0xFF || rule.event_filter == hist.event) {
                        if (rule.tid_filter == 0 || rule.tid_filter == hist.task_id) {
                            ++match_count;
                        }
                    }
                }

                if (match_count < rule.threshold_count) continue;
            }

            // 执行规则动作
            execute_rule_action_(rule, entry);
        }
    }

    // ---- 查询接口 ----

    int get_entry_count() const {
        int total = write_count_;
        return (total > kRingCapacity) ? kRingCapacity : total;
    }

    const AuditEntry* get_entry(int index) const {
        if (index < 0 || index >= kRingCapacity) return nullptr;
        return &ring_buffer_[index];
    }

    // ---- 统计信息 ----

    uint32_t get_total_records() const { return write_count_; }
    int      get_active_rule_count() const {
        int c = 0;
        for (int i = 0; i < kMaxRules; ++i) {
            if (rules_[i].active) ++c;
        }
        return c;
    }

    static constexpr int get_max_rules() { return kMaxRules; }

private:
    AuditEngine() = default;

    static constexpr int kRingCapacity = 128;
    static constexpr int kMaxRules     = 16;

    AuditEntry    ring_buffer_[kRingCapacity]{};
    volatile int  write_count_ = 0;
    AuditLogNode  audit_log_node_;
    bool          initialized_ = false;

    AuditRule     rules_[kMaxRules]{};

    // 写入环形缓冲区（关中断保护，ISR 安全）
    void push_entry_(const AuditEntry& entry) {
        IrqGuard guard;
        int idx = write_count_ % kRingCapacity;
        ring_buffer_[idx] = entry;
        ++write_count_;

        // 在关中断保护下评估规则
        evaluate_rules_(entry, guard);
    }

    // 规则评估（在关中断中调用）
    void evaluate_rules_(const AuditEntry& entry, IrqGuard& guard) {
        // 复制到栈再做匹配，避免长时间关中断
        AuditRule local_rules[kMaxRules];
        for (int i = 0; i < kMaxRules; ++i) {
            local_rules[i] = rules_[i];
        }
        // 可以在这里开中断，但为了简单先保持

        for (int i = 0; i < kMaxRules; ++i) {
            const AuditRule& rule = local_rules[i];
            if (!rule.active) continue;

            if (rule.event_filter != 0xFF && rule.event_filter != entry.event) continue;
            if (rule.tid_filter != 0 && rule.tid_filter != entry.task_id) continue;

            if (rule.path_pattern[0] != '\0' && entry.path[0] != '\0') {
                if (!path_match_(entry.path, rule.path_pattern)) continue;
            }

            if (rule.threshold_count > 0) {
                uint32_t window_ticks = rule.threshold_ms; // ms ≈ ticks
                int match_count = 0;
                int total = write_count_;
                int limit = (total > kRingCapacity) ? kRingCapacity : total;
                for (int j = 0; j < limit; ++j) {
                    int idx = (total - 1 - j) % kRingCapacity;
                    if (idx < 0) idx += kRingCapacity;
                    const AuditEntry& hist = ring_buffer_[idx];
                    if (entry.timestamp - hist.timestamp > window_ticks) break;

                    if (rule.event_filter == 0xFF || rule.event_filter == hist.event) {
                        if (rule.tid_filter == 0 || rule.tid_filter == hist.task_id) {
                            ++match_count;
                        }
                    }
                }
                if (match_count < rule.threshold_count) continue;
            }

            execute_rule_action_(rule, entry);
        }
        (void)guard; // guard 析构时自动开中断
    }

    void execute_rule_action_(const AuditRule& rule, const AuditEntry& entry) {
        // 生成告警条目
        AuditEntry alert{};
        alert.timestamp = get_tick_();
        alert.task_id   = entry.task_id;
        alert.event     = static_cast<uint8_t>(AuditEvent::AuditAlert);
        alert.result    = 1; // 告警
        alert.arg0      = entry.event;   // 触发事件
        alert.arg1      = entry.arg0;    // 触发参数
        alert.path[0]   = '\0';

        // 将告警写入缓冲区
        int idx = write_count_ % kRingCapacity;
        ring_buffer_[idx] = alert;
        ++write_count_;

        // 如果是终止任务动作
        if (rule.action >= 2) {
            TaskControlBlock* tcb = Scheduler::instance().get_task_by_id(entry.task_id);
            if (tcb && tcb->state != TaskState::Terminated) {
                Scheduler::instance().set_task_state(tcb->id, TaskState::Terminated);
            }
        }
    }

    // 简单通配符路径匹配（支持 *）
    static bool path_match_(const char* path, const char* pattern) {
        if (!path || !pattern) return false;

        int pi = 0, si = 0;
        int star = -1, match = 0;

        while (path[pi]) {
            if (pattern[si] == '*') {
                star = si;
                match = pi;
                ++si;
            } else if (pattern[si] == path[pi] ||
                       (pattern[si] == '?' && path[pi])) {
                ++pi;
                ++si;
            } else if (star >= 0) {
                si = star + 1;
                ++match;
                pi = match;
            } else {
                return false;
            }
        }

        while (pattern[si] == '*') ++si;
        return pattern[si] == '\0';
    }

    static uint32_t get_tick_() {
        extern volatile uint32_t tick_count;
        return tick_count;
    }

    static uint16_t get_current_tid_() {
        TaskControlBlock* cur = Scheduler::instance().get_current_tcb();
        return cur ? static_cast<uint16_t>(cur->id) : 0xFFFF;
    }

    static AuditEvent map_svc_to_event_(uint8_t svc) {
        switch (svc) {
            case 0x01: return AuditEvent::SysPrint;
            case 0x02: return AuditEvent::SysYield;
            case 0x03: return AuditEvent::SysSleep;
            case 0x08: return AuditEvent::SysCapDerive;
            case 0x09: return AuditEvent::SysCapMint;
            case 0x0A: return AuditEvent::SysCapRevoke;
            case 0x0B: return AuditEvent::SysCapDelete;
            case 0x0C: return AuditEvent::SysCapGrant;
            case 0x14: return AuditEvent::SysKill;
            case 0x15: return AuditEvent::SysSigAction;
            case 0x16: return AuditEvent::SysSigMask;
            case 0x10: return AuditEvent::SysIpcCall;
            case 0x11: return AuditEvent::SysIpcRecv;
            case 0x12: return AuditEvent::SysIpcReply;
            default:   return AuditEvent::SysUnknown;
        }
    }
};

// ============================================================
// 审计 Lua 绑定（供 MiniProgramEngine 调用）
// ============================================================
class AuditLuaBinding {
public:
    // 注册到 Lua VM 的 aurora.audit 命名空间
    static void register_bindings(void* lua_state);

private:
    // Lua C 函数实现声明（实现在文件末尾）
    static int lua_add_rule(void* L);
    static int lua_clear_rules(void* L);
    static int lua_get_alert_count(void* L);
};

// 内联便捷宏：用于在 posix.cpp / interrupts.cpp 中快速集成审计钩子
// 用法: AUDIT_HOOK_OPEN(path, fd, flags);
#ifndef BOARD_MCU_STM32L031K6
#define AUDIT_HOOK_OPEN(path, fd, flags)   AuditEngine::instance().record_open(path, fd, flags)
#define AUDIT_HOOK_WRITE(fd, n)            AuditEngine::instance().record_write(fd, n)
#define AUDIT_HOOK_READ(fd, n)             AuditEngine::instance().record_read(fd, n)
#define AUDIT_HOOK_CLOSE(fd, ret)          AuditEngine::instance().record_close(fd, ret)
#define AUDIT_HOOK_SVC(svc, result)        AuditEngine::instance().record_svc(svc, result)
#else
#define AUDIT_HOOK_OPEN(path, fd, flags)   do {} while(0)
#define AUDIT_HOOK_WRITE(fd, n)            do {} while(0)
#define AUDIT_HOOK_READ(fd, n)             do {} while(0)
#define AUDIT_HOOK_CLOSE(fd, ret)          do {} while(0)
#define AUDIT_HOOK_SVC(svc, result)        do {} while(0)
#endif

#endif // AURORA_KERNEL_AUDIT_HPP
