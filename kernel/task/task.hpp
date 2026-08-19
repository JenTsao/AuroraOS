#ifndef TASK_HPP
#define TASK_HPP

#include <stdint.h>
#include <stddef.h>
#include "../core/arch_api.hpp" // 引入底层架构 HAL 接口
#include "../mm/mpu.hpp"
#include "../mm/vasp.hpp"
#include "../core/cspace.hpp"
#include "../core/ipc.hpp" // For SandboxDescriptor
#include "../core/kernel_object.hpp"
#include "../core/placement_new.hpp"

class Mutex; // 前向声明，用于优先级继承

extern "C" bool frame_scheduler_is_task_allowed(uint8_t priority);

// Weak default: no-op if watchdog_manager.hpp is not linked.
// Overridden by WatchdogManager::instance().on_schedule() when present.
void watchdog_feed(uint32_t task_priority);

// ============================================================
// 1. 定义标准 RTOS 优先级阶梯 (数值越大，优先级越高)
//    遵循 C++ Core Guidelines Enum.3: 使用 enum class 强类型枚举
// ============================================================
enum class TaskPriority : uint8_t {
    Idle = 0,    // 最低优先级：仅供系统空闲进程使用
    Low = 1,     // 低优先级：后台计算、非实时操作
    Normal = 2,  // 默认优先级：普通前台业务
    High = 3,    // 高优先级：交互 Shell 等
    Realtime = 4 // 最高硬实时优先级：底层网络帧拦截等
};

enum class TaskPrivilege : uint32_t {
    Kernel = 0,
    User = 1
};

enum class TaskState {
    Ready,
    Running,
    Sleeping,
    Blocked_On_Notify,
    Terminated,
    Suspended,
    Unallocated
};

// POSIX 标准信号定义
// 宿主环境（<signal.h>）会把这些名字定义为宏，导致 `constexpr int SIGINT = 2;`
// 被预处理器展开成 `constexpr int 2 = 2;` 语法错误，故先解除宏定义。
#ifdef SIGINT
#undef SIGINT
#endif
#ifdef SIGKILL
#undef SIGKILL
#endif
#ifdef SIGALRM
#undef SIGALRM
#endif
#ifdef SIGUSR1
#undef SIGUSR1
#endif
constexpr int SIGINT = 2;   // 中断信号 (Ctrl+C)
constexpr int SIGKILL = 9;  // 强制终止
constexpr int SIGALRM = 14; // 定时器超时报警
constexpr int SIGUSR1 = 10; // 用户自定义信号 1

using SignalHandler = void (*)(int sig);

// sigprocmask 操作常量
#ifdef SIG_BLOCK
#undef SIG_BLOCK
#endif
#ifdef SIG_UNBLOCK
#undef SIG_UNBLOCK
#endif
#ifdef SIG_SETMASK
#undef SIG_SETMASK
#endif
constexpr int SIG_BLOCK = 0;
constexpr int SIG_UNBLOCK = 1;
constexpr int SIG_SETMASK = 2;

// SA_flags (simplified for now)
// NOTE: glibc <signal.h> defines SA_RESETHAND / SA_NODEFER as macros; undefine them
// first so our own constants compile on host builds (where <signal.h> is pulled in).
#ifdef SA_RESETHAND
#undef SA_RESETHAND
#endif
#ifdef SA_NODEFER
#undef SA_NODEFER
#endif
constexpr int SA_RESETHAND = 1;
constexpr int SA_NODEFER = 2;

// NOTE: renamed from `sigaction` to avoid redefinition with the POSIX <signal.h>
// `struct sigaction` on host builds. This is our own simplified signal-action type.
struct SignalAction {
    SignalHandler sa_handler;
    uint32_t sa_mask;
    int sa_flags;
};

// 工具宏：用于快速操作信号掩码
#define sigaddset(mask_ptr, signo) (*(mask_ptr) |= (1U << (signo)))
#define sigdelset(mask_ptr, signo) (*(mask_ptr) &= ~(1U << (signo)))
#define sigemptyset(mask_ptr) (*(mask_ptr) = 0)
#define sigfillset(mask_ptr) (*(mask_ptr) = 0xFFFFFFFFU)
#define sigismember(mask_ptr, signo) (((*(mask_ptr)) & (1U << (signo))) != 0)

// TaskContext: Hardware & Task Execution State
struct TaskContext {
    uint32_t* stack_ptr;        // 任务当前栈顶指针（由 PendSV 保存/恢复）
    uint32_t privilege;         // 特权级 (0: Kernel, 1: User)
    void (*entry_point)();      // 任务入口函数
    int errno_val;              // 线程本地 errno
    uint32_t* stack_canary_ptr; // 指向栈底 word（stack_space[0]）
};

// SchedulerContext: Scheduling & synchronization state
struct SchedulerContext {
    TaskState state;               // 任务状态机
    uint32_t id;                   // 任务唯一 ID
    uint32_t sleep_ticks;          // 剩余休眠 Tick 数
    TaskPriority base_priority;    // 基础优先级
    TaskPriority current_priority; // 动态优先级（用于优先级继承）
    int32_t next_ready;            // 动态优先级队列: 下一个就绪任务索引
    int32_t prev_ready;            // 动态优先级队列: 上一个就绪任务索引
    void* held_mutexes;            // 持有的互斥锁链表头
    Mutex* waiting_on_mutex;       // 当前正在等待的互斥锁
};

// MemoryContext: Memory isolation state
struct MemoryContext {
    uintptr_t stack_base;          // 栈基址（用于 MPU/MMU/Sandbox）
    uint8_t size_pow2;             // 栈大小的 2 的幂次方（用于 MPU）
    SandboxDescriptor mpu_sandbox; // MPU Sandbox 描述符
    uintptr_t pgdir_base;          // 页表根目录物理基址（用于 MMU TTBR0/satp）
    auroraos::kernel::VirtualAddressSpace* vasp; // 关联的虚拟地址空间对象
};

// IpcContext: Communication state
struct IpcContext {
    auroraos::kernel::IpcState state;
    auroraos::kernel::IpcStatus status;          // IPC 操作结果状态 (Ok, Timeout, WouldBlock 等)
    TaskControlBlock* blocked_next;              // IPC 端点等待队列链表
    void* msg_buf;
    void* reply_buf;
    uint32_t msg_len;
    uint32_t max_len;
    uint32_t sender_id;
    uint32_t receiver_id;
    uint32_t badge;                              // seL4 风格权能 Badge 认证标记 (防伪造)
    uint32_t msg_type;                           // 消息 Label / Protocol Type ID
    uint32_t label_filter;                       // 接收端 Label 过滤器 (0 = 接收任意消息)
    uint32_t notify_value;                       // 32 位专有通知值
    bool notify_pending;                         // 是否有未处理的通知
    auroraos::kernel::Endpoint* waiting_endpoint;// 当前正在等待的端点 (用于超时/撤销清理)
};

// SecurityContext: Capability & Signal state
struct SecurityContext {
    auroraos::kernel::Capability cspace[auroraos::kernel::MAX_CSPACE_SLOTS];
    uint32_t pending_signals;     // 待处理信号位图
    uint32_t signal_mask;         // 被屏蔽的信号位图
    SignalAction sig_actions[16]; // 信号配置表
};

// TaskControlBlock: composed from modular contexts
struct TaskControlBlock : public auroraos::kernel::KernelObject {
    TaskControlBlock() : auroraos::kernel::KernelObject(auroraos::kernel::ObjectType::Task), task{}, scheduler{},
                         memory{}, ipc{}, security{} {}

    TaskContext task;
    SchedulerContext scheduler;
    MemoryContext memory;
    IpcContext ipc;
    SecurityContext security;

    static constexpr int NUM_SIG_ACTIONS = 16;

protected:
    void destroy() override;
};

// PendSV 汇编硬编码 [rN, #0] 读 stack_ptr、[rN, #4] 读 privilege；
#if !defined(ARCH_AARCH64) && !defined(AURORA_HOST_TEST)
static_assert(sizeof(uint32_t*) == 4, "PendSV requires 4-byte pointer at offset 0");
static_assert(offsetof(TaskContext, privilege) == 4, "PendSV LDR [rx, #4] expects privilege at offset 4");
#endif

// 前向声明：供 PendSV 汇编读取的两个全局 TCB 指针
// 遵循 I.2: 最小化非 const 全局变量，此处为架构必需
extern "C" {
extern TaskControlBlock* volatile g_current_tcb_ptr;
extern TaskControlBlock* volatile g_next_tcb_ptr;
extern volatile uint32_t g_switch_start_cycle;
}

struct IrqGuard {
    uint32_t primask_;

    IrqGuard() {
        primask_ = Arch::irq_save();
    }

    ~IrqGuard() {
        Arch::irq_restore(primask_);
    }

    IrqGuard(const IrqGuard&) = delete;
    IrqGuard& operator=(const IrqGuard&) = delete;
};

class Scheduler {
public:
    // 栈水印哨兵值。0xDEADBEEF 是业界主流调试哨兵：字节序无关、人眼可识别、便于 Crash dump 判读
    static constexpr uint32_t STACK_CANARY = 0xDEADBEEFu;

#ifdef CONFIG_MAX_TASKS
    static constexpr int MAX_TASKS = CONFIG_MAX_TASKS;
#else
    static constexpr int MAX_TASKS = 16;
#endif

#ifdef CONFIG_TICK_RATE_HZ
    static constexpr uint32_t TICK_RATE_HZ = CONFIG_TICK_RATE_HZ;
#else
    static constexpr uint32_t TICK_RATE_HZ = 1000;
#endif

    static constexpr int get_max_tasks() {
        return MAX_TASKS;
    }

    static constexpr uint32_t get_tick_rate_hz() {
        return TICK_RATE_HZ;
    }

    // 单例：遵循 I.3，避免多实例造成调度器状态不一致
    static Scheduler& instance() {
        static Scheduler sched;
        return sched;
    }

    void init() {
        task_count = 0;
        started_ = false;
        ready_bitmask = 0;
        for (int i = 0; i < 5; i++)
            ready_head[i] = -1;
        for (int i = 0; i < MAX_TASKS; i++) {
            tasks[i].scheduler.next_ready = -1;
            tasks[i].scheduler.prev_ready = -1;
            tasks[i].scheduler.state = TaskState::Unallocated;
        }
    }

    void push_ready(uint32_t task_index) {
        IrqGuard guard;
        if (task_index >= MAX_TASKS)
            return; // 运行时越界保护
        TaskControlBlock& tcb = tasks[task_index];
        uint8_t prio = static_cast<uint8_t>(tcb.scheduler.current_priority);
        int32_t head = ready_head[prio];

        if (head == -1) {
            ready_head[prio] = task_index;
            tcb.scheduler.next_ready = task_index;
            tcb.scheduler.prev_ready = task_index;
            ready_bitmask |= static_cast<uint8_t>(1u << prio);
        } else {
            TaskControlBlock& head_tcb = tasks[head];
            int32_t tail = head_tcb.scheduler.prev_ready;
            TaskControlBlock& tail_tcb = tasks[tail];

            tail_tcb.scheduler.next_ready = task_index;
            tcb.scheduler.prev_ready = tail;
            tcb.scheduler.next_ready = head;
            head_tcb.scheduler.prev_ready = task_index;
        }
    }

    void remove_ready(uint32_t task_index) {
        IrqGuard guard;
        if (task_index >= MAX_TASKS)
            return; // 运行时越界保护
        TaskControlBlock& tcb = tasks[task_index];
        uint8_t prio = static_cast<uint8_t>(tcb.scheduler.current_priority);

        if (tcb.scheduler.next_ready == -1)
            return; // Not in queue

        if (static_cast<uint32_t>(tcb.scheduler.next_ready) == task_index) {
            // Only element
            ready_head[prio] = -1;
            ready_bitmask &= static_cast<uint8_t>(~(1u << prio));
        } else {
            TaskControlBlock& prev_tcb = tasks[tcb.scheduler.prev_ready];
            TaskControlBlock& next_tcb = tasks[tcb.scheduler.next_ready];
            prev_tcb.scheduler.next_ready = tcb.scheduler.next_ready;
            next_tcb.scheduler.prev_ready = tcb.scheduler.prev_ready;
            if (ready_head[prio] == static_cast<int32_t>(task_index)) {
                ready_head[prio] = tcb.scheduler.next_ready;
            }
        }
        tcb.scheduler.next_ready = -1;
        tcb.scheduler.prev_ready = -1;
    }

    void set_task_state(uint32_t id, TaskState new_state) {
        IrqGuard guard;
        if (id >= MAX_TASKS || id >= task_count)
            return;
        TaskControlBlock& tcb = tasks[id];
        if (tcb.scheduler.state == new_state)
            return;

        if (tcb.scheduler.state == TaskState::Ready) {
            remove_ready(id);
        }
        tcb.scheduler.state = new_state;
        if (new_state == TaskState::Ready) {
            push_ready(id);
        }
    }

    void set_task_priority(uint32_t id, TaskPriority new_prio) {
        IrqGuard guard;
        if (id >= MAX_TASKS || id >= task_count)
            return;
        TaskControlBlock& tcb = tasks[id];
        if (tcb.scheduler.current_priority == new_prio)
            return;

        bool was_ready = (tcb.scheduler.state == TaskState::Ready);
        if (was_ready) {
            remove_ready(id);
        }
        tcb.scheduler.current_priority = new_prio;
        if (was_ready) {
            push_ready(id);
        }
    }

    // 创建任务时指定优先级（默认 Normal），遵循 F.15: 提供具名参数
    // 返回值：成功时返回新任务的 TCB 指针（供调用方获取其真实句柄，
    // 例如 lwIP sys_thread_new() 需要返回"新创建线程"而非当前线程的句柄）；
    // 任务表已满（达到 MAX_TASKS）时返回 nullptr，调用方必须检查该返回值，
    // 不能像过去那样静默吞掉创建失败。
    TaskControlBlock* create_task(void (*task_entry)(void), uint32_t* stack_space, uint32_t stack_size,
                                  TaskPriority prio = TaskPriority::Normal, uint8_t size_pow2 = 0,
                                  TaskPrivilege priv = TaskPrivilege::Kernel) { // 默认为内核特权

        int free_idx = -1;
        for (int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i].scheduler.state == TaskState::Unallocated) {
                free_idx = i;
                break;
            }
        }
        if (free_idx == -1)
            return nullptr; // No free slots

        if (static_cast<uint32_t>(free_idx) >= task_count) {
            task_count = free_idx + 1; // Update high-water mark
        }

        TaskControlBlock& tcb = tasks[free_idx];
        // Note: KernelObject ref_count is already 1 upon creation/allocation if we construct it,
        // but since this is a statically allocated array, we must re-initialize its KernelObject state.
        new (&tcb) TaskControlBlock();

        tcb.scheduler.id = free_idx;
        tcb.scheduler.state = TaskState::Ready;
        tcb.scheduler.sleep_ticks = 0;
        tcb.scheduler.base_priority = prio;
        tcb.scheduler.current_priority = prio;
        tcb.task.entry_point = task_entry;
        tcb.task.privilege = static_cast<uint32_t>(priv);
        tcb.memory.stack_base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(stack_space));
        tcb.memory.size_pow2 = size_pow2;
        tcb.memory.pgdir_base = 0;
        tcb.memory.vasp = nullptr;

        tcb.memory.mpu_sandbox.stack_base = tcb.memory.stack_base;
        tcb.memory.mpu_sandbox.size_pow2 = size_pow2;
        tcb.memory.mpu_sandbox.version = 1;
        tcb.memory.mpu_sandbox.seal();

        tcb.scheduler.next_ready = -1;
        tcb.scheduler.prev_ready = -1;

        // 初始化任务通知与信号管理
        tcb.ipc.notify_value = 0;
        tcb.ipc.notify_pending = false;

        tcb.security.pending_signals = 0;
        tcb.security.signal_mask = 0; // 默认不屏蔽
        for (int i = 0; i < TaskControlBlock::NUM_SIG_ACTIONS; i++) {
            tcb.security.sig_actions[i].sa_handler = nullptr;
            tcb.security.sig_actions[i].sa_mask = 0;
            tcb.security.sig_actions[i].sa_flags = 0;
        }
        tcb.scheduler.held_mutexes = nullptr;
        tcb.scheduler.waiting_on_mutex = nullptr;

        tcb.task.errno_val = 0; // 初始化线程本地 errno

        // 初始化 IPC 与 CSpace
        tcb.ipc.state = auroraos::kernel::IpcState::Ready;
        tcb.ipc.status = auroraos::kernel::IpcStatus::Ok;
        tcb.ipc.blocked_next = nullptr;
        tcb.ipc.waiting_endpoint = nullptr;
        tcb.ipc.badge = 0;
        tcb.ipc.msg_type = 0; // raw/untyped
        tcb.ipc.label_filter = 0;
        for (int i = 0; i < auroraos::kernel::MAX_CSPACE_SLOTS; i++) {
            tcb.security.cspace[i].type = auroraos::kernel::CapType::Null;
            tcb.security.cspace[i].rights = {0, 0, 0, 0};
            tcb.security.cspace[i].badge = 0;
            tcb.security.cspace[i].object = nullptr;
        }

        // 【栈水印】在栈底（数组首元素，栈向下增长所以首地址 = 最低地址）写入哨兵
        tcb.task.stack_canary_ptr = stack_space; // stack_space[0] = 栈底
        if (tcb.task.stack_canary_ptr != nullptr) {
            *tcb.task.stack_canary_ptr = STACK_CANARY;
        }

        // 调用 HAL 接口完成 Cortex-M4 栈帧伪造，与具体架构解耦
        tcb.task.stack_ptr = Arch::init_thread_stack(task_entry, stack_space, stack_size);
        push_ready(free_idx);
        return &tcb;
    }

    // ========================================================
    // 【核心改造】调度器在任务切换时，自动检查并分发待处理信号
    // ========================================================
    void dispatch_signals(TaskControlBlock* tcb) {
        if (!tcb || tcb->security.pending_signals == 0)
            return;

        IrqGuard guard; // 防止与 ISR / 重入的 schedule() 对信号队列的并发访问

        // 快速检测：是否有未屏蔽的信号，避免无效轮转和跨调度的重复处理
        uint32_t actionable = (tcb->security.pending_signals & ~(tcb->security.signal_mask)) |
                              (tcb->security.pending_signals & (1U << SIGKILL));
        if (!actionable)
            return;

        // 存在可处理信号，原地提取并清空待处理位
        uint32_t pending = tcb->security.pending_signals;
        tcb->security.pending_signals = 0;

        for (int sig = 1; sig < TaskControlBlock::NUM_SIG_ACTIONS; sig++) {
            if (pending & (1U << sig)) {
                // 如果该信号被屏蔽，且不是 SIGKILL，那么不处理，重新排入位图
                if (sig != SIGKILL && sigismember(&tcb->security.signal_mask, sig)) {
                    tcb->security.pending_signals |= (1U << sig);
                    continue;
                }

                if (sig == SIGKILL) {
                    set_task_state(tcb->scheduler.id, TaskState::Terminated);
                    return; // 终止后不再执行其它处理函数
                }

                const auto& action = tcb->security.sig_actions[sig];
                if (action.sa_handler) {
                    uint32_t old_mask = tcb->security.signal_mask;
                    tcb->security.signal_mask |= action.sa_mask;

                    if (!(action.sa_flags & SA_NODEFER)) {
                        sigaddset(&tcb->security.signal_mask, sig);
                    }

                    action.sa_handler(sig);

                    if (action.sa_flags & SA_RESETHAND) {
                        tcb->security.sig_actions[sig].sa_handler = nullptr;
                    }

                    tcb->security.signal_mask = old_mask;
                }
            }
        }
    }

    void signal_dispatch(TaskControlBlock* tcb) {
        dispatch_signals(tcb);
    }

    bool send_signal(uint32_t target_id, int sig) {
        if (target_id >= task_count || sig <= 0 || sig >= 32)
            return false;
        TaskControlBlock& target = tasks[target_id];
        if (target.scheduler.state == TaskState::Terminated)
            return false;

        target.security.pending_signals |= (1U << sig);
        if (target.scheduler.state == TaskState::Sleeping || target.scheduler.state == TaskState::Blocked_On_Notify) {
            set_task_state(target.scheduler.id, TaskState::Ready);
        }
        return true;
    }

    // =========================================================================
    // 任务自愿降级（进入用户态）
    // 关闭中断，原子更新 TCB 记录与硬件 CONTROL 寄存器，然后恢复中断。
    // 消除上下文切换时的竞争状态。
    // =========================================================================
    void drop_self_privilege() {
        IrqGuard guard;
        uint32_t current_task_id = g_current_tcb_ptr ? g_current_tcb_ptr->scheduler.id : 0;
        TaskControlBlock& tcb = tasks[current_task_id];
        tcb.task.privilege = 1; // 1 = User privilege
        Arch::set_privilege(1);
    }

    // =========================================================================
    // 基于优先级的抢占式调度算法 — O(1) 动态优先级队列
    //
    // 阶段一: 从 ready_bitmask 快速定位最高优先级
    // 阶段二: 时间片轮转
    // 阶段三: 发起上下文切换
    // =========================================================================
    void schedule() {
        if (!started_ || task_count <= 1)
            return;

        g_switch_start_cycle = Arch::get_cycle();

        uint32_t current_task_id = g_current_tcb_ptr ? g_current_tcb_ptr->scheduler.id : 0;

        // 【安全信号拦截点】
        dispatch_signals(&tasks[current_task_id]);

        // 【修复 BUG #3】检查当前任务是否已被 dispatch_signals 终止（如 SIGKILL）。
        // 若已终止，跳过时间片轮转（轮转会访问已终止任务的链表指针，虽然不会崩溃但
        // 属于不安全的代码路径），直接进入任务选择。
        bool current_terminated = (tasks[current_task_id].scheduler.state == TaskState::Terminated);

        if (!current_terminated) {
            // ── 时间片轮转：如果当前任务仍然就绪且处于队首，将其移至队尾
            IrqGuard guard;
            if (tasks[current_task_id].scheduler.state == TaskState::Ready) {
                uint8_t p = static_cast<uint8_t>(tasks[current_task_id].scheduler.current_priority);
                if (ready_head[p] == static_cast<int32_t>(current_task_id)) {
                    ready_head[p] = tasks[current_task_id].scheduler.next_ready;
                }
            }
        }

        // ── O(1) 寻找最高可运行优先级（硬件 CLZ 指令加速） ──
        uint32_t next_task = current_task_id;
        {
            IrqGuard guard;
            uint32_t mask = ready_bitmask;
            while (mask != 0) {
                int p = Arch::find_highest_bit(mask);
                // 【蓝河帧感知拦截】
                if (frame_scheduler_is_task_allowed(p)) {
                    next_task = ready_head[p];
                    break;
                }
                mask &= ~(1u << p);
            }

            // ── 一级兜底：若帧感知拦截了所有优先级，则退回到 Idle ──
            if (next_task == current_task_id) {
                if (ready_bitmask & (1 << 0)) {
                    next_task = ready_head[0];
                }
            }

            // ── 【修复 BUG #2】二级兜底：确保选中的任务确实处于 Ready 状态 ──
            // 原实现：线性扫描索引最低的任务，忽略优先级。
            // 修复：按优先级从高到低通过 CLZ 硬件加速扫描，选择优先级最高且处于 Ready 的任务。
            if (tasks[next_task].scheduler.state != TaskState::Ready) {
                uint32_t mask_fallback = ready_bitmask;
                while (mask_fallback != 0) {
                    int p = Arch::find_highest_bit(mask_fallback);
                    // 验证该优先级的队首任务确实处于 Ready
                    uint32_t candidate = ready_head[p];
                    if (candidate < task_count && tasks[candidate].scheduler.state == TaskState::Ready) {
                        next_task = candidate;
                        goto found_ready; // 跳出双层搜索
                    }
                    // 队头不可用，扫描该优先级链表
                    if (tasks[candidate].scheduler.next_ready != -1) {
                        uint32_t iter = tasks[candidate].scheduler.next_ready;
                        while (iter != static_cast<uint32_t>(ready_head[p]) && iter != static_cast<uint32_t>(-1)) {
                            if (tasks[iter].scheduler.state == TaskState::Ready) {
                                next_task = iter;
                                goto found_ready;
                            }
                            iter = tasks[iter].scheduler.next_ready;
                        }
                    }
                    mask_fallback &= ~(1u << p);
                }
            found_ready:;
            }
        } // Close IrqGuard block

        // ── 上下文切换 ──
        // 【修复 BUG #7】触发 PendSV 进行实际上下文切换。
        // current_task_index 相关的同步竞态已消除，现在由底层 PendSV_Handler 唯一更新 g_current_tcb_ptr。
        if (next_task != current_task_id) {
            IrqGuard guard;
            g_next_tcb_ptr = &tasks[next_task];
            if (tasks[next_task].memory.pgdir_base != 0) {
                Arch::switch_address_space(tasks[next_task].memory.pgdir_base);
            }
            Arch::trigger_context_switch();
        }

        // 心跳喂狗：每次调度都喂，卡死时 SysTick 停止触发自然超时复位
        watchdog_feed(static_cast<uint32_t>(tasks[current_task_id].scheduler.current_priority));
    }

    // 主动休眠：将当前任务挂起，立刻调度次高优先级任务接管 CPU
    void sleep_ms(uint32_t ms) {
        // 注意：sleep() 仅在任务上下文调用，无需屏蔽中断
        // SysTick 只会检查 sleeping 状态，不会修改当前任务的字段
        TaskControlBlock* current = get_current_tcb();
        // 转换 ms → ticks，向上取整避免 sleep_ms(1) 在低 tick 频率下变成 0 tick
        current->scheduler.sleep_ticks =
            static_cast<uint32_t>((static_cast<uint64_t>(ms) * TICK_RATE_HZ + 999u) / 1000u);
        set_task_state(current->scheduler.id, TaskState::Sleeping);
        schedule(); // 状态更新后立即让出 CPU
    }

    // 由 SysTick 中断调用：滴答计数器驱动唤醒逻辑
    void tick_update() {
        for (uint32_t i = 0; i < task_count; i++) {
            // 【栈水印检测】先验哨兵再处理休眠
            if (tasks[i].task.stack_canary_ptr != nullptr && *tasks[i].task.stack_canary_ptr != STACK_CANARY &&
                tasks[i].scheduler.state != TaskState::Terminated) {
                // 栈底哨兵被覆盖 — 立即终止该任务，防止内核数据被破坏
                // 必须通过 set_task_state() 以正确从就绪链表摘除该任务节点，
                // 防止已损坏的任务继续被 schedule() 调度上处理器执行。
                set_task_state(i, TaskState::Terminated);
                // SecurityMonitor 将在下一次心跳周期检测到并记录
                continue;
            }

            if (tasks[i].scheduler.state == TaskState::Sleeping) {
                if (tasks[i].scheduler.sleep_ticks > 0) {
                    tasks[i].scheduler.sleep_ticks--;
                }
                if (tasks[i].scheduler.sleep_ticks == 0) {
                    set_task_state(i, TaskState::Ready);
                }
            } else if (tasks[i].ipc.state == auroraos::kernel::IpcState::Sending ||
                       tasks[i].ipc.state == auroraos::kernel::IpcState::Receiving ||
                       tasks[i].ipc.state == auroraos::kernel::IpcState::ReplyBlocked) {
                if (tasks[i].scheduler.sleep_ticks > 0) {
                    tasks[i].scheduler.sleep_ticks--;
                    if (tasks[i].scheduler.sleep_ticks == 0) {
                        if (tasks[i].ipc.waiting_endpoint != nullptr) {
                            tasks[i].ipc.waiting_endpoint->cancel_waiter(&tasks[i]);
                        } else {
                            tasks[i].ipc.state = auroraos::kernel::IpcState::Ready;
                            tasks[i].ipc.status = auroraos::kernel::IpcStatus::Timeout;
                            set_task_state(i, TaskState::Ready);
                        }
                    }
                }
            }
        }
    }

    // 1. 预测未来：扫描所有休眠/等待超时中的任务，找出最快要醒来的那个时间差
    uint32_t get_expected_idle_ticks() {
        uint32_t min_ticks = 0xFFFFFFFF; // 初始设为无限大

        for (uint32_t i = 0; i < task_count; i++) {
            if (tasks[i].scheduler.sleep_ticks > 0 && tasks[i].scheduler.sleep_ticks < min_ticks) {
                min_ticks = tasks[i].scheduler.sleep_ticks;
            }
        }
        return min_ticks;
    }

    // 2. 补偿跳过的时间：Tickless 睡眠醒来后，批量扣除休眠任务与 IPC 超时任务的等待时间
    void compensate_ticks(uint32_t skipped_ticks) {
        for (uint32_t i = 0; i < task_count; i++) {
            if (tasks[i].scheduler.state == TaskState::Sleeping && tasks[i].scheduler.sleep_ticks > 0) {
                if (tasks[i].scheduler.sleep_ticks > skipped_ticks) {
                    tasks[i].scheduler.sleep_ticks -= skipped_ticks;
                } else {
                    tasks[i].scheduler.sleep_ticks = 0;
                    set_task_state(i, TaskState::Ready);
                }
            } else if ((tasks[i].ipc.state == auroraos::kernel::IpcState::Sending ||
                        tasks[i].ipc.state == auroraos::kernel::IpcState::Receiving ||
                        tasks[i].ipc.state == auroraos::kernel::IpcState::ReplyBlocked) &&
                       tasks[i].scheduler.sleep_ticks > 0) {
                if (tasks[i].scheduler.sleep_ticks > skipped_ticks) {
                    tasks[i].scheduler.sleep_ticks -= skipped_ticks;
                } else {
                    tasks[i].scheduler.sleep_ticks = 0;
                    if (tasks[i].ipc.waiting_endpoint != nullptr) {
                        tasks[i].ipc.waiting_endpoint->cancel_waiter(&tasks[i]);
                    } else {
                        tasks[i].ipc.state = auroraos::kernel::IpcState::Ready;
                        tasks[i].ipc.status = auroraos::kernel::IpcStatus::Timeout;
                        set_task_state(i, TaskState::Ready);
                    }
                }
            }
        }
        // 注意：全局的系统 tick 计数器也需要加上 skipped_ticks
    }

    // 遵循 F.16: 返回裸指针仅表示非所有权观察（调度器拥有 TCB 数组）
    TaskControlBlock* get_current_tcb() {
        return g_current_tcb_ptr;
    }

    int get_task_count() const {
        return task_count;
    }

    TaskControlBlock* get_task(int index) {
        if (index >= 0 && static_cast<uint32_t>(index) < task_count)
            return &tasks[index];
        return nullptr;
    }

    TaskControlBlock* get_task_by_id(uint32_t id) {
        if (id < task_count)
            return &tasks[id];
        return nullptr;
    }

    void free_task(TaskControlBlock* tcb) {
        IrqGuard guard;
        if (!tcb)
            return;

        // Clean up virtual address space if allocated
        if (tcb->memory.vasp) {
            delete tcb->memory.vasp;
            tcb->memory.vasp = nullptr;
            tcb->memory.pgdir_base = 0;
        }

        // Remove from ready queues if needed
        if (tcb->scheduler.state == TaskState::Ready) {
            remove_ready(tcb->scheduler.id);
        }

        // Update state to Unallocated so it can be recycled
        tcb->scheduler.state = TaskState::Unallocated;
    }

    // Testing hook
    void set_started(bool s) {
        started_ = s;
    }

    /// 利用硬件 CLZ 指令 O(1) 获取当前就绪队列中最高优先级，若无就绪任务返回 -1
    int32_t get_highest_ready_priority() const noexcept {
        return Arch::find_highest_bit(ready_bitmask);
    }

    // =========================================================================
    // start(): 从特权 main 上下文启动调度器，跳入第一个任务
    // 架构相关的 PSP/CONTROL 切换与栈帧恢复已封装在 Arch::start_first_task()
    // 中，调度器逻辑层不再感知具体异常返回码或内联汇编
    // =========================================================================
    [[noreturn]] void start() {
        started_ = true;
        g_current_tcb_ptr = &tasks[0];
        g_next_tcb_ptr = &tasks[0];

        if (tasks[0].memory.pgdir_base != 0) {
            Arch::switch_address_space(tasks[0].memory.pgdir_base);
        }

        // 配置 SysTick 系统心跳（默认 1000Hz → 每 1ms 一次中断）
        // 必须在 start_first_task() 内部的 cpsie i 之前完成：
        // 此时全局中断仍关闭，配置安全；开中断后 SysTick 立即开始产生周期心跳
        Arch::systick_init(TICK_RATE_HZ);

        Arch::start_first_task(g_current_tcb_ptr->task.stack_ptr, tasks[0].task.entry_point, tasks[0].task.privilege);
    }

private:
    Scheduler() {
        for (int i = 0; i < 5; i++)
            ready_head[i] = -1;
        for (int i = 0; i < MAX_TASKS; i++) {
            tasks[i].scheduler.state = TaskState::Unallocated;
        }
    }

    TaskControlBlock tasks[MAX_TASKS]{};
    uint32_t task_count = 0;
    bool started_ = false;

    int32_t ready_head[5];     // Head of ready list for each priority level (0-4)
    uint8_t ready_bitmask = 0; // Bitmask of priorities that have ready tasks
};

inline void TaskControlBlock::destroy() {
    Scheduler::instance().free_task(this);
}

#endif // TASK_HPP
