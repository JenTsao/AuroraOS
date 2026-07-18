#ifndef TASK_HPP
#define TASK_HPP

#include <stdint.h>
#include <stddef.h>
#include "arch_api.hpp" // 引入底层架构 HAL 接口
#include "mpu.hpp"
#include "cspace.hpp"
#include "ipc.hpp"      // For SandboxDescriptor

class Mutex; // 前向声明，用于优先级继承

extern "C" bool frame_scheduler_is_task_allowed(uint8_t priority);

// ============================================================
// 1. 定义标准 RTOS 优先级阶梯 (数值越大，优先级越高)
//    遵循 C++ Core Guidelines Enum.3: 使用 enum class 强类型枚举
// ============================================================
enum class TaskPriority : uint8_t {
    Idle     = 0,  // 最低优先级：仅供系统空闲进程使用
    Low      = 1,  // 低优先级：后台计算、非实时操作
    Normal   = 2,  // 默认优先级：普通前台业务
    High     = 3,  // 高优先级：交互 Shell 等
    Realtime = 4   // 最高硬实时优先级：底层网络帧拦截等
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
    Suspended
};

// POSIX 标准信号定义
constexpr int SIGINT  = 2;  // 中断信号 (Ctrl+C)
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
constexpr int SIG_BLOCK   = 0;
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
constexpr int SA_NODEFER   = 2;

// NOTE: renamed from `sigaction` to avoid redefinition with the POSIX <signal.h>
// `struct sigaction` on host builds. This is our own simplified signal-action type.
struct SignalAction {
    SignalHandler sa_handler;
    uint32_t      sa_mask;
    int           sa_flags;
};

// 工具宏：用于快速操作信号掩码
#define sigaddset(mask_ptr, signo) (*(mask_ptr) |= (1U << (signo)))
#define sigdelset(mask_ptr, signo) (*(mask_ptr) &= ~(1U << (signo)))
#define sigemptyset(mask_ptr)      (*(mask_ptr) = 0)
#define sigfillset(mask_ptr)       (*(mask_ptr) = 0xFFFFFFFFU)
#define sigismember(mask_ptr, signo) (((*(mask_ptr)) & (1U << (signo))) != 0)

// TaskControlBlock: POD 结构体，保存任务的完整上下文快照
// 遵循 C.2: 若需要不变量则使用 class，此处为纯数据故用 struct
struct TaskControlBlock {
    uint32_t*    stack_ptr;       // 任务当前栈顶指针（由 PendSV 保存/恢复）
    uint32_t     privilege;       // 特权级 (0: Kernel, 1: User)
    void         (*entry_point)(); // 任务入口函数（供 start() 引导跳入第一个任务用）
    TaskState    state;           // 任务状态机
    uint32_t     id;              // 任务唯一 ID
    uint32_t     sleep_ticks;     // 剩余休眠 Tick 数
    TaskPriority base_priority;   // 基础优先级
    TaskPriority current_priority;// 动态优先级（用于优先级继承）
    uint32_t     stack_base;      // 栈基址（用于 MPU）
    uint8_t      size_pow2;       // 栈大小的 2 的幂次方（用于 MPU）

    int8_t       next_ready;      // 动态优先级队列: 下一个就绪任务索引
    int8_t       prev_ready;      // 动态优先级队列: 上一个就绪任务索引

    // ========================================================
    // 1. 【FreeRTOS 任务通知】零开销 TCB 内置字段
    // ========================================================
    uint32_t  notify_value;     // 32 位专有通知值
    bool      notify_pending;   // 是否有未处理的通知

    // ========================================================
    // 2. 【POSIX 信号】信号队列与屏蔽 (修复合并丢失问题)
    // ========================================================
    static constexpr int MAX_QUEUED_SIGNALS = 32;
    static constexpr int NUM_SIG_ACTIONS    = 16;
    uint8_t       signal_queue[MAX_QUEUED_SIGNALS];
    uint8_t       sig_head;
    uint8_t       sig_tail;
    uint8_t       sig_count;
    uint32_t      signal_mask;              // 被屏蔽的信号位图（32 位）
    SignalAction  sig_actions[NUM_SIG_ACTIONS]; // 信号配置表（仅 16 项，需运行时越界检查）
    
    void*         held_mutexes;             // 持有的互斥锁链表头 (for PI)
    Mutex*        waiting_on_mutex;         // 当前正在等待的互斥锁 (for transitive PI)

    // ========================================================
    // 3. 【栈水印（Stack Canary）】
    //    create_task() 在栈底（最低地址）写入 STACK_CANARY 哨兵字。
    //    tick_update() 每 tick 检测其是否被覆盖。
    // ========================================================
    uint32_t* stack_canary_ptr;  // 指向栈底 word（stack_space[0]）

    // ========================================================
    // 4. 【MPU Sandbox】(Verified descriptor with CRC32)
    // ========================================================
    SandboxDescriptor mpu_sandbox;
    
    // ========================================================
    // 5. 【MMU VASP】(Virtual Address Space Page Directory)
    // ========================================================
    uintptr_t pgdir_base;
#ifdef ARCH_AARCH64
    void* vasp_ptr;
#endif

    // ========================================================
    // 6. 【seL4 Capability & IPC 模型】
    // ========================================================
    auroraos::kernel::Capability cspace[auroraos::kernel::MAX_CSPACE_SLOTS];
    auroraos::kernel::IpcState ipc_state;
    
    // IPC 端点等待队列链表
    TaskControlBlock* ipc_blocked_next;
    
    void* ipc_msg_buf;
    void* ipc_reply_buf;
    uint32_t ipc_msg_len;
    uint32_t ipc_max_len;
    uint32_t ipc_sender_id;   // 记录发送方 ID (或接收到的 Sender ID)
    uint32_t ipc_receiver_id; // 记录配对的接收方 ID，用于验证 Reply 权限

    // ========================================================
    // 7. 【POSIX 兼容层】
    // ========================================================
    int errno_val;           // 线程本地 errno
};

// PendSV 汇编硬编码 [rN, #0] 读 stack_ptr、[rN, #4] 读 privilege；
// AArch64 使用独立的 svc #0 上下文切换路径，不受此约束
#ifndef ARCH_AARCH64
static_assert(sizeof(uint32_t*) == 4,
    "PendSV requires 4-byte pointer at TCB offset 0");
static_assert(offsetof(TaskControlBlock, privilege) == 4,
    "PendSV LDR [rx, #4] expects privilege at offset 4");
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
    IrqGuard() { primask_ = Arch::irq_save(); }
    ~IrqGuard() { Arch::irq_restore(primask_); }
    IrqGuard(const IrqGuard&) = delete;
    IrqGuard& operator=(const IrqGuard&) = delete;
};


class Scheduler {
public:
    // 单例：遵循 I.3，避免多实例造成调度器状态不一致
    static Scheduler& instance() {
        static Scheduler sched;
        return sched;
    }

    void init() {
        current_task_index = 0;
        task_count = 0;
        started_ = false;
        ready_bitmask = 0;
        for (int i = 0; i < 5; i++) ready_head[i] = -1;
        for (int i = 0; i < MAX_TASKS; i++) {
            tasks[i].next_ready = -1;
            tasks[i].prev_ready = -1;
        }
    }

    void push_ready(uint32_t task_index) {
        IrqGuard guard;
        TaskControlBlock& tcb = tasks[task_index];
        uint8_t prio = static_cast<uint8_t>(tcb.current_priority);
        int8_t head = ready_head[prio];
        
        if (head == -1) {
            ready_head[prio] = task_index;
            tcb.next_ready = task_index;
            tcb.prev_ready = task_index;
            ready_bitmask |= (1 << prio);
        } else {
            TaskControlBlock& head_tcb = tasks[head];
            int8_t tail = head_tcb.prev_ready;
            TaskControlBlock& tail_tcb = tasks[tail];
            
            tail_tcb.next_ready = task_index;
            tcb.prev_ready = tail;
            tcb.next_ready = head;
            head_tcb.prev_ready = task_index;
        }
    }

    void remove_ready(uint32_t task_index) {
        IrqGuard guard;
        TaskControlBlock& tcb = tasks[task_index];
        uint8_t prio = static_cast<uint8_t>(tcb.current_priority);
        
        if (tcb.next_ready == -1) return; // Not in queue
        
        if (static_cast<uint32_t>(tcb.next_ready) == task_index) { 
            // Only element
            ready_head[prio] = -1;
            ready_bitmask &= ~(1 << prio);
        } else {
            TaskControlBlock& prev_tcb = tasks[tcb.prev_ready];
            TaskControlBlock& next_tcb = tasks[tcb.next_ready];
            prev_tcb.next_ready = tcb.next_ready;
            next_tcb.prev_ready = tcb.prev_ready;
            if (ready_head[prio] == static_cast<int8_t>(task_index)) {
                ready_head[prio] = tcb.next_ready;
            }
        }
        tcb.next_ready = -1;
        tcb.prev_ready = -1;
    }

    void set_task_state(uint32_t id, TaskState new_state) {
        IrqGuard guard;
        if (id >= task_count) return;
        TaskControlBlock& tcb = tasks[id];
        if (tcb.state == new_state) return;

        if (tcb.state == TaskState::Ready) {
            remove_ready(id);
        }
        tcb.state = new_state;
        if (new_state == TaskState::Ready) {
            push_ready(id);
        }
    }

    void set_task_priority(uint32_t id, TaskPriority new_prio) {
        IrqGuard guard;
        if (id >= task_count) return;
        TaskControlBlock& tcb = tasks[id];
        if (tcb.current_priority == new_prio) return;

        bool was_ready = (tcb.state == TaskState::Ready);
        if (was_ready) {
            remove_ready(id);
        }
        tcb.current_priority = new_prio;
        if (was_ready) {
            push_ready(id);
        }
    }

    // 创建任务时指定优先级（默认 Normal），遵循 F.15: 提供具名参数
    // 返回值：成功时返回新任务的 TCB 指针（供调用方获取其真实句柄，
    // 例如 lwIP sys_thread_new() 需要返回"新创建线程"而非当前线程的句柄）；
    // 任务表已满（达到 MAX_TASKS）时返回 nullptr，调用方必须检查该返回值，
    // 不能像过去那样静默吞掉创建失败。
    static constexpr int get_max_tasks() {
#ifdef CONFIG_MAX_TASKS
        return CONFIG_MAX_TASKS;
#else
        return 16;
#endif
    }

    TaskControlBlock* create_task(void (*task_entry)(void),
                     uint32_t* stack_space,
                     uint32_t  stack_size,
                     TaskPriority prio = TaskPriority::Normal,
                     uint8_t size_pow2 = 0,
                     TaskPrivilege priv = TaskPrivilege::Kernel) { // 默认为内核特权
        if (task_count >= MAX_TASKS) return nullptr;

        TaskControlBlock& tcb = tasks[task_count];
        tcb.id          = task_count;
        tcb.state       = TaskState::Ready;
        tcb.sleep_ticks = 0;
        tcb.base_priority = prio;
        tcb.current_priority = prio;
        tcb.entry_point = task_entry;
        tcb.privilege = static_cast<uint32_t>(priv);
        tcb.stack_base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(stack_space));
        tcb.size_pow2 = size_pow2;
        
        tcb.mpu_sandbox.stack_base = tcb.stack_base;
        tcb.mpu_sandbox.size_pow2  = size_pow2;
        tcb.mpu_sandbox.version    = 1;
        tcb.mpu_sandbox.seal();

        tcb.next_ready = -1;
        tcb.prev_ready = -1;

        // 初始化任务通知与信号管理
        tcb.notify_value = 0;
        tcb.notify_pending = false;
        
        tcb.sig_head = 0;
        tcb.sig_tail = 0;
        tcb.sig_count = 0;
        tcb.signal_mask = 0; // 默认不屏蔽
        for (int i = 0; i < TaskControlBlock::NUM_SIG_ACTIONS; i++) {
            tcb.sig_actions[i].sa_handler = nullptr;
            tcb.sig_actions[i].sa_mask = 0;
            tcb.sig_actions[i].sa_flags = 0;
        }
        tcb.held_mutexes = nullptr;
        tcb.waiting_on_mutex = nullptr;

        tcb.errno_val = 0; // 初始化线程本地 errno

        // 初始化 IPC 与 CSpace
        tcb.ipc_state = auroraos::kernel::IpcState::Ready;
        tcb.ipc_blocked_next = nullptr;
        for (int i = 0; i < auroraos::kernel::MAX_CSPACE_SLOTS; i++) {
            tcb.cspace[i].type = auroraos::kernel::CapType::Null;
            tcb.cspace[i].rights = {0, 0, 0, 0};
            tcb.cspace[i].badge = 0;
            tcb.cspace[i].object = nullptr;
        }

        // 【栈水印】在栈底（数组首元素，栈向下增长所以首地址 = 最低地址）写入哨兵
        static constexpr uint32_t STACK_CANARY = 0xDEADBEEFu;
        tcb.stack_canary_ptr = stack_space;  // stack_space[0] = 栈底
        if (tcb.stack_canary_ptr != nullptr) {
            *tcb.stack_canary_ptr = STACK_CANARY;
        }

        // 调用 HAL 接口完成 Cortex-M4 栈帧伪造，与具体架构解耦
        tcb.stack_ptr = Arch::init_thread_stack(task_entry, stack_space, stack_size);
        push_ready(task_count);
        task_count++;
        return &tcb;
    }

    // ========================================================
    // 【核心改造】调度器在任务切换时，自动检查并分发待处理信号
    // ========================================================
    void dispatch_signals(TaskControlBlock* tcb) {
        if (!tcb || tcb->sig_count == 0) return;
        
        IrqGuard guard; // 防止与 ISR / 重入的 schedule() 对信号队列的并发访问
        
        int initial_count = tcb->sig_count;
        
        for (int i = 0; i < initial_count; i++) {
            if (tcb->sig_count == 0) break;
            
            uint8_t sig = tcb->signal_queue[tcb->sig_head];
            
            // 如果该信号被屏蔽，且不是 SIGKILL，那么不处理，跳过它（将其重新排入队列末尾）
            if (sig != SIGKILL && sigismember(&tcb->signal_mask, sig)) {
                tcb->sig_head = (tcb->sig_head + 1) % TaskControlBlock::MAX_QUEUED_SIGNALS;
                tcb->signal_queue[tcb->sig_tail] = sig;
                tcb->sig_tail = (tcb->sig_tail + 1) % TaskControlBlock::MAX_QUEUED_SIGNALS;
                continue;
            }
            
            // 可以处理，将其出队
            tcb->sig_head = (tcb->sig_head + 1) % TaskControlBlock::MAX_QUEUED_SIGNALS;
            tcb->sig_count--;
            
            if (sig == SIGKILL) {
                set_task_state(tcb->id, TaskState::Terminated);
                return; // 终止后不再执行其它处理函数
            }
            
            // 越界检查：sig_actions 仅 NUM_SIG_ACTIONS 项，但 signal_mask 支持 32 位
            if (sig >= TaskControlBlock::NUM_SIG_ACTIONS) continue;

            const auto& action = tcb->sig_actions[sig];
            if (action.sa_handler) {
                // 如果设置了 sa_mask，在处理信号期间叠加到 signal_mask (简化版，未实现恢复)
                // 正规实现应当在 user-space trampoline 恢复，由于目前在调度器同步执行，
                // 我们在内核态保存恢复
                uint32_t old_mask = tcb->signal_mask;
                tcb->signal_mask |= action.sa_mask;
                
                if (!(action.sa_flags & SA_NODEFER)) {
                    sigaddset(&tcb->signal_mask, sig);
                }

                action.sa_handler(sig);
                
                if (action.sa_flags & SA_RESETHAND) {
                    tcb->sig_actions[sig].sa_handler = nullptr;
                }
                
                tcb->signal_mask = old_mask;
            }
        }
    }

    // =========================================================================
    // 基于优先级的抢占式调度算法 — O(1) 动态优先级队列
    //
    // 阶段一: 从 ready_bitmask 快速定位最高优先级
    // 阶段二: 时间片轮转
    // 阶段三: 发起上下文切换
    // =========================================================================
    void schedule() {
        if (!started_ || task_count <= 1) return;

        g_switch_start_cycle = Arch::get_cycle();

        // 【安全信号拦截点】
        dispatch_signals(&tasks[current_task_index]);

        // ── 时间片轮转：如果当前任务仍然就绪且处于队首，将其移至队尾
        {
            IrqGuard guard;
            if (tasks[current_task_index].state == TaskState::Ready) {
                uint8_t p = static_cast<uint8_t>(tasks[current_task_index].current_priority);
                if (ready_head[p] == static_cast<int8_t>(current_task_index)) {
                    ready_head[p] = tasks[current_task_index].next_ready;
                }
            }
        }

        // ── O(1) 寻找最高可运行优先级 ──
        uint32_t next_task = current_task_index;

        for (int p = 4; p >= 0; p--) {
            if (ready_bitmask & (1 << p)) {
                // 【蓝河帧感知拦截】
                if (frame_scheduler_is_task_allowed(p)) {
                    next_task = ready_head[p];
                    break;
                }
            }
        }

        // ── 一级兜底：若帧感知拦截了所有优先级，则退回到 Idle ──
        if (next_task == current_task_index) {
            if (ready_bitmask & (1 << 0)) {
                next_task = ready_head[0];
            }
        }

        // ── 二级兜底：确保选中的任务确实处于 Ready 状态 ──
        // （例如 Idle 被 SIGKILL 终止、或当前任务刚被 dispatch_signals 终止）
        if (tasks[next_task].state != TaskState::Ready) {
            for (uint32_t i = 0; i < task_count; i++) {
                if (tasks[i].state == TaskState::Ready) {
                    next_task = i;
                    break;
                }
            }
        }

        // ── 上下文切换 ──
        if (next_task != current_task_index) {
            Arch::disable_interrupts();
            g_current_tcb_ptr = &tasks[current_task_index];
            current_task_index = next_task;
            g_next_tcb_ptr = &tasks[current_task_index];
            
            Arch::enable_interrupts();
            Arch::trigger_context_switch();
        }
    }

    // 主动休眠：将当前任务挂起，立刻调度次高优先级任务接管 CPU
    void sleep_ms(uint32_t ms) {
        // 注意：sleep() 仅在任务上下文调用，无需屏蔽中断
        // SysTick 只会检查 sleeping 状态，不会修改当前任务的字段
        TaskControlBlock* current = get_current_tcb();
        // 转换 ms → ticks，向上取整避免 sleep_ms(1) 在低 tick 频率下变成 0 tick
        current->sleep_ticks = static_cast<uint32_t>(
            (static_cast<uint64_t>(ms) * TICK_RATE_HZ + 999u) / 1000u);
        set_task_state(current->id, TaskState::Sleeping);
        schedule(); // 状态更新后立即让出 CPU
    }

    // 由 SysTick 中断调用：滴答计数器驱动唤醒逻辑
    void tick_update() {
        for (uint32_t i = 0; i < task_count; i++) {
            // 【栈水印检测】先验哨兵再处理休眠
            if (tasks[i].stack_canary_ptr != nullptr &&
                *tasks[i].stack_canary_ptr != 0xDEADBEEFu &&
                tasks[i].state != TaskState::Terminated) {
                // 栈底哨兵被覆盖 — 立即终止该任务，防止内核数据被破坏
                // 必须通过 set_task_state() 以正确从就绪链表摘除该任务节点，
                // 防止已损坏的任务继续被 schedule() 调度上处理器执行。
                set_task_state(i, TaskState::Terminated);
                // SecurityMonitor 将在下一次心跳周期检测到并记录
                continue;
            }

            if (tasks[i].state == TaskState::Sleeping) {
                if (tasks[i].sleep_ticks > 0) {
                    tasks[i].sleep_ticks--;
                }
                if (tasks[i].sleep_ticks == 0) {
                    set_task_state(i, TaskState::Ready);
                }
            }
        }
    }

    // 1. 预测未来：扫描所有休眠中的任务，找出最快要醒来的那个时间差
    uint32_t get_expected_idle_ticks() {
        uint32_t min_ticks = 0xFFFFFFFF; // 初始设为无限大
        
        for (uint32_t i = 0; i < task_count; i++) {
            if (tasks[i].state == TaskState::Sleeping) {
                if (tasks[i].sleep_ticks > 0 && tasks[i].sleep_ticks < min_ticks) {
                    min_ticks = tasks[i].sleep_ticks;
                }
            }
        }
        return min_ticks;
    }

    // 2. 补偿跳过的时间：Tickless 睡眠醒来后，批量扣除休眠任务的等待时间
    void compensate_ticks(uint32_t skipped_ticks) {
        for (uint32_t i = 0; i < task_count; i++) {
            if (tasks[i].state == TaskState::Sleeping && tasks[i].sleep_ticks > 0) {
                if (tasks[i].sleep_ticks > skipped_ticks) {
                    tasks[i].sleep_ticks -= skipped_ticks;
                } else {
                    tasks[i].sleep_ticks = 0;
                    set_task_state(i, TaskState::Ready);
                }
            }
        }
        // 注意：全局的系统 tick 计数器也需要加上 skipped_ticks
    }

    // 遵循 F.16: 返回裸指针仅表示非所有权观察（调度器拥有 TCB 数组）
    TaskControlBlock* get_current_tcb() {
        if (!started_) return nullptr;
        return &tasks[current_task_index];
    }
    

    int get_task_count() const { return task_count; }
    TaskControlBlock* get_task(int index) { 
        if (index >= 0 && static_cast<uint32_t>(index) < task_count) return &tasks[index]; 
        return nullptr;
    }
    TaskControlBlock* get_task_by_id(uint32_t id) {
        if (id < task_count) return &tasks[id];
        return nullptr;
    }
    
    // Testing hook
    void set_started(bool s) { started_ = s; }

    // =========================================================================
    // start(): 从特权 main 上下文启动调度器，跳入第一个任务
    // 架构相关的 PSP/CONTROL 切换与栈帧恢复已封装在 Arch::start_first_task()
    // 中，调度器逻辑层不再感知具体异常返回码或内联汇编
    // =========================================================================
    [[noreturn]] void start() {
        started_ = true;
        g_current_tcb_ptr = &tasks[current_task_index];
        g_next_tcb_ptr    = &tasks[current_task_index];

        // 配置 SysTick 系统心跳（默认 1000Hz → 每 1ms 一次中断）
        // 必须在 start_first_task() 内部的 cpsie i 之前完成：
        // 此时全局中断仍关闭，配置安全；开中断后 SysTick 立即开始产生周期心跳
        Arch::systick_init(TICK_RATE_HZ);

        Arch::start_first_task(g_current_tcb_ptr->stack_ptr,
                               tasks[current_task_index].entry_point,
                               tasks[current_task_index].privilege);
    }

private:
    Scheduler() {
        for (int i = 0; i < 5; i++) ready_head[i] = -1;
    }
#ifdef CONFIG_MAX_TASKS
    static constexpr int MAX_TASKS = CONFIG_MAX_TASKS;
#else
    static constexpr int MAX_TASKS = 16;
#endif
    static_assert(MAX_TASKS <= 127,
        "MAX_TASKS exceeds int8_t range of next_ready/prev_ready (max 127)");

#ifdef CONFIG_TICK_RATE_HZ
    static constexpr uint32_t TICK_RATE_HZ = CONFIG_TICK_RATE_HZ;
#else
    static constexpr uint32_t TICK_RATE_HZ = 1000;
#endif

    TaskControlBlock tasks[MAX_TASKS]{};
    uint32_t current_task_index = 0;
    uint32_t task_count = 0;
    bool started_ = false;
    
    int8_t ready_head[5]; // Head of ready list for each priority level (0-4)
    uint8_t ready_bitmask = 0; // Bitmask of priorities that have ready tasks
};

#endif // TASK_HPP