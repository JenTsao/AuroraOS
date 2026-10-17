#ifndef TASK_HPP
#define TASK_HPP

#include <stdint.h>
#include <stddef.h>
#include "../core/arch_api.hpp"        // 引入底层架构 HAL 接口
#include "../mm/memory_attributes.hpp" // 高速 RAM / DTCM / CCMRAM 属性
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

// Weak default: no-op if process_timer.cpp is not linked.
// Overridden by ProcessTimerManager::instance().cleanup_task_timers() when present.
extern "C" void kernel_cleanup_task_timers(uint32_t task_id);

// Weak default: no-op if mutex subsystem is not linked.
// Overridden by kernel/core/mutex.cpp: force-releases all mutexes held by the
// task and detaches it from any mutex wait queue (termination safety cleanup).
extern "C" void kernel_release_task_mutexes(uint32_t task_id);

// ============================================================
// 1. 定义标准 RTOS 优先级阶梯 (0 ~ 31，数值越大，优先级越高)
//    遵循 C++ Core Guidelines Enum.3: 使用 enum class 强类型枚举
//    位图调度器单字 uint32_t 完美映射 32 级优先级，利用硬件 CLZ 单指令调度
// ============================================================
enum class TaskPriority : uint8_t {
    // ── 系统基础级 (0 ~ 7) ──────────────────────────────────
    Idle = 0,        // 最低优先级：仅供系统空闲进程与待机模式使用
    Background = 2,  // 后台低速计算、非关键日志写入、堆内存碎片整理
    Low = 4,         // 低优先级：后台扫描、离线指标汇总、数据预处理
    Maintenance = 6, // 系统维护：慢速 flash 磨损均衡、垃圾回收

    // ── 业务与交互级 (8 ~ 17) ────────────────────────────────
    Normal = 8,       // 默认业务优先级：普通前台应用、POSIX 进程、VFS 文件操作
    Service = 10,     // 系统后台服务：通知中心、电源策略管理、守护进程
    UI = 12,          // UI 渲染与图层合成：2D 渲染器、表盘动效驱动
    Interactive = 14, // 交互事件响应：触控手势滑动、按键消抖派发
    High = 16,        // 高优先级交互：Shell 终端命令行、OTA 固件通信通道

    // ── 实时驱动与外设流处理 (18 ~ 31，细分子系统硬实时响应) ─
    Net_TX = 18,  // 网络发送：lwIP 发送缓冲队列推进
    Net_RX = 20,  // 网络接收：网卡/BLE 接收数据包低延迟解包与分发
    Sensor = 24,  // 传感器采集：PPG 心率/IMU 计步/ECG/防跌倒高频流式算法
    Audio = 28,   // 音频实时流：语音采集/DAC 播报/抗爆音硬件 DMA 缓冲
    ISR_DPC = 30, // 中断延迟过程调用 (DPC) / 中断底半部快速任务化执行
    Realtime = 31 // 最高硬实时：关键安全监控 (SecurityMonitor)、系统硬实时看门狗
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
    TaskState state;                // 任务状态机
    uint32_t id;                    // 任务唯一 ID
    uint32_t sleep_ticks;           // 剩余休眠 Tick 数
    TaskPriority base_priority;     // 基础优先级
    TaskPriority current_priority;  // 动态优先级（用于优先级继承）
    int32_t next_ready;             // 动态优先级队列: 下一个就绪任务索引
    int32_t prev_ready;             // 动态优先级队列: 上一个就绪任务索引
    void* held_mutexes;             // 持有的互斥锁链表头
    Mutex* waiting_on_mutex;        // 当前正在等待的互斥锁
    uint32_t total_runtime_ticks;   // 累计运行时间片 (Ticks)
    uint32_t total_runtime_cycles;  // 累计运行 CPU 周期 (Cycles)
    uint32_t switch_count;          // 被调度切换选中的总次数
    uint32_t last_dispatched_cycle; // 上次被选入运行的起始周期
};

// MemoryContext: Memory isolation state
struct MemoryContext {
    uintptr_t stack_base;                        // 栈基址（用于 MPU/MMU/Sandbox）
    uint8_t size_pow2;                           // 栈大小的 2 的幂次方（用于 MPU）
    uint8_t subregion_disable;                   // MPU 子区域禁用掩码 (SRD)
    SandboxDescriptor mpu_sandbox;               // MPU Sandbox 描述符
    uintptr_t pgdir_base;                        // 页表根目录物理基址（用于 MMU TTBR0/satp）
    auroraos::kernel::VirtualAddressSpace* vasp; // 关联的虚拟地址空间对象
};

// IpcContext: Communication state
struct IpcContext {
    auroraos::kernel::IpcState state;
    auroraos::kernel::IpcStatus status; // IPC 操作结果状态 (Ok, Timeout, WouldBlock 等)
    TaskControlBlock* blocked_next;     // IPC 端点等待队列链表
    void* msg_buf;
    void* reply_buf;
    uint32_t msg_len;
    uint32_t max_len;
    uint32_t sender_id;
    uint32_t receiver_id;
    uint32_t badge;                               // seL4 风格权能 Badge 认证标记 (防伪造)
    uint32_t msg_type;                            // 消息 Label / Protocol Type ID
    uint32_t label_filter;                        // 接收端 Label 过滤器 (0 = 接收任意消息)
    uint32_t notify_value;                        // 32 位专有通知值
    bool notify_pending;                          // 是否有未处理的通知
    auroraos::kernel::Endpoint* waiting_endpoint; // 当前正在等待的端点 (用于超时/撤销清理)

    // 【阻塞 IPC 的返回值投递】任务在 SVC 内阻塞时 PendSV 还没执行，它的异常
    // 帧仍完整留在该任务自己的栈上。ipc_ret_slot 指向该帧内"系统调用返回值
    // 寄存器"槽（Cortex-M: 硬件压栈的 r0；RV32: 保存上下文中的 a0），由各自
    // 的 SVC/trap 入口装填；终态唤醒点经 Endpoint::finish_waiter 写回真实的
    // IpcStatus，被唤醒任务看到的才是最终结果而不是一直停在 Blocked。
    // 所有权: 指向被阻塞任务自身的栈内存，阻塞期间无人改写；投递完成或本次
    // 调用并未真正挂起时必须立即置空 (Endpoint::settle_ipc_return_slot)。
    uint32_t* ipc_ret_slot;
    // 阻塞路径下推迟写出的用户出参：sender_id/badge 只有唤醒时才正确
    uint32_t* pending_sender_id_out;
    uint32_t* pending_badge_out;
};

// SecurityContext: Capability & Signal state
struct SecurityContext {
    auroraos::kernel::Capability cspace[auroraos::kernel::MAX_CSPACE_SLOTS];
    uint16_t occupied_mask;       // 16 槽位占用位图 (bit i = 1 表示 slot i 已分配/非 Null)
    uint16_t reserved;            // 对齐填充
    uint32_t pending_signals;     // 待处理信号位图
    uint32_t signal_mask;         // 被屏蔽的信号位图
    SignalAction sig_actions[16]; // 信号配置表
};

// TaskControlBlock: composed from modular contexts, 8-byte aligned for Cortex-M LDRD/STRD and DTCM
//
// ⚠️ PendSV 汇编 ABI（cm0plus/cm3/cm4f/cm7 的切换代码均硬编码偏移）：
//    TaskContext 必须是第二个子对象（offset 12），即 [tcb+12]=stack_ptr、
//    [tcb+16]=privilege。原因：TCB 继承 KernelObject（多态，Thread 能力指向
//    TCB，见 wifi_monitor_task 的 cap_insert）→ 按 Itanium ABI 主基类规则，
//    动态基类 KernelObject 携带 vptr 占据 offset 0，非动态的 TaskContext 被
//    排在其后。历史包袱：旧汇编硬编码 [tcb+0]/[tcb+4]，恰逢把 vptr 当
//    stack_ptr 读、把 ObjectType 字节当 privilege 读——单任务路径（lm3s）
//    侥幸不崩，Cortex-M7 多任务轮转实测把虚表指针当栈顶恢复 → INVSTATE
//    HardFault。现由下方 static_assert 在编译期锁定该契约。
struct alignas(8) TaskControlBlock : public TaskContext, public auroraos::kernel::KernelObject {
    TaskControlBlock()
        : TaskContext{}, auroraos::kernel::KernelObject(auroraos::kernel::ObjectType::Task), scheduler{}, memory{},
          ipc{}, security{} {}

    SchedulerContext scheduler;
    MemoryContext memory;
    IpcContext ipc;
    SecurityContext security;

    static constexpr int NUM_SIG_ACTIONS = 16;

protected:
    void destroy() override;
};

// PendSV 汇编硬编码 [rN, #12] 读 stack_ptr、[rN, #16] 读 privilege（vptr+KernelObject 占据低 12 字节）；
#if !defined(ARCH_AARCH64) && !defined(AURORA_HOST_TEST)
static_assert(sizeof(uint32_t*) == 4, "PendSV requires 4-byte pointer at offset 0");
static_assert(offsetof(TaskContext, privilege) == 4, "TaskContext 内部: privilege 应位于 offset 4");
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof" // TCB 非标准布局，GCC 支持该 offsetof 用法
static_assert(offsetof(TaskControlBlock, stack_ptr) == 12,
              "PendSV LDR [rx, #12] expects stack_ptr at TCB offset 12 (vptr + KernelObject occupy the low bytes)");
static_assert(offsetof(TaskControlBlock, privilege) == 16, "PendSV LDR [rx, #16] expects privilege at TCB offset 16");
#pragma GCC diagnostic pop
#endif

// 前向声明：供 PendSV 汇编读取的两个全局 TCB 指针
// 遵循 I.2: 最小化非 const 全局变量，此处为架构必需
extern "C" {
extern TaskControlBlock* volatile g_current_tcb_ptr;
extern TaskControlBlock* volatile g_next_tcb_ptr;
extern volatile uint32_t g_switch_start_cycle;
}

// ============================================================
// RAII 临界区保护守卫 (IrqGuard)
// 在 Cortex-M3/M4/M4F 上利用 BASEPRI 进行优先级掩蔽 (仅屏蔽普通中断，保留硬实时中断)；
// 在 Cortex-M0+ 上利用 PRIMASK 全局关中断；在 RISC-V/AArch64 上利用 mstatus/daif 保存恢复。
// ============================================================
struct IrqGuard {
    uint32_t saved_flags_;

    IrqGuard() {
        saved_flags_ = Arch::irq_save();
    }

    ~IrqGuard() {
        Arch::irq_restore(saved_flags_);
    }

    IrqGuard(const IrqGuard&) = delete;
    IrqGuard& operator=(const IrqGuard&) = delete;
};

class alignas(8) Scheduler {
public:
    // 栈水印哨兵值。0xDEADBEEF 是业界主流调试哨兵：字节序无关、人眼可识别、便于 Crash dump 判读
    static constexpr uint32_t STACK_CANARY = 0xDEADBEEFu;

#ifdef CONFIG_MAX_TASKS
    static constexpr int MAX_TASKS = CONFIG_MAX_TASKS;
#else
    static constexpr int MAX_TASKS = 16;
#endif

    // Mutex / Semaphore 的 wait_mask_ 是 uint32_t 位掩码，按 task id 索引
    // （见 kernel/core/mutex.hpp、kernel/core/semaphore.hpp 中的 `1U << id`）。
    // 超过 32 时移位将越出掩码宽度，导致优先级继承(PIP)计数被静默破坏。
    // Kconfig 中已用 range 1..32 约束；此处作为第二道防线，覆盖所有
    // 未经过 genconfig 的构建路径（外部 -D、手写测试编译等）。
    static_assert(MAX_TASKS <= 32, "MAX_TASKS must be <= 32: Mutex/Semaphore wait_mask_ is uint32_t");
    static_assert(MAX_TASKS >= 1, "MAX_TASKS must be >= 1");

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
        for (uint32_t i = 0; i < NUM_PRIORITIES; i++)
            ready_head[i] = -1;
        for (int i = 0; i < MAX_TASKS; i++) {
            tasks[i].scheduler.next_ready = -1;
            tasks[i].scheduler.prev_ready = -1;
            tasks[i].scheduler.state = TaskState::Unallocated;
            tasks[i].scheduler.total_runtime_ticks = 0;
            tasks[i].scheduler.total_runtime_cycles = 0;
            tasks[i].scheduler.switch_count = 0;
            tasks[i].scheduler.last_dispatched_cycle = 0;
        }
        total_switches_ = 0;
        active_ticks_ = 0;
        idle_ticks_ = 0;
        g_current_tcb_ptr = nullptr;
        g_next_tcb_ptr = nullptr;
    }
};

#endif // TASK_HPP
