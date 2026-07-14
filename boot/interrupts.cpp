#include "interrupts.hpp"
#include "uart.h"
#include "task.hpp"
#include "syscall.hpp"
#include "timer.hpp"
#include "work_queue.hpp"
#include "mpu.hpp"
#include "../kernel/cspace.hpp"
#include "../kernel/ipc.hpp"
#include "frame_scheduler_v2.hpp"
#include "../metrics/metrics.hpp"

volatile uint32_t isr_enter_cycle = 0;

// ────────────────────────────────────────────────────────────────
// SyscallValidator — kernel-side parameter validation for SVC calls.
// All functions are noexcept; they never throw or call user code.
// ────────────────────────────────────────────────────────────────
namespace SyscallValidator {

// Flash region boundaries exposed by the linker script.
// Defined as weak to allow host-test stubs to override them.
extern "C" __attribute__((weak)) uint32_t _flash_start;
extern "C" __attribute__((weak)) uint32_t _flash_end;

// Validate that [ptr, ptr+len) lies entirely within either:
//   (a) the calling task’s stack region, or
//   (b) the read-only Flash segment (for string literals).
// Returns true if the range is safe to dereference in kernel context.
[[nodiscard]] inline bool validate_user_ptr(
        const void* ptr, size_t len,
        uintptr_t task_stack_base, size_t task_stack_size) noexcept
{
    if (!ptr) return false;
    const uintptr_t p   = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t end = p + len;
    // Integer wrap-around check
    if (end < p) return false;

    // (a) Within the task’s own stack
    if (task_stack_size > 0) {
        const bool in_stack = (p >= task_stack_base) &&
                              (end <= task_stack_base + task_stack_size);
        if (in_stack) return true;
    }

    // (b) Within read-only Flash (for string literals passed as SYS_PRINT arg)
    const uintptr_t flash_s = reinterpret_cast<uintptr_t>(&_flash_start);
    const uintptr_t flash_e = reinterpret_cast<uintptr_t>(&_flash_end);
    if (flash_s != flash_e) {  // linker symbols valid
        const bool in_flash = (p >= flash_s) && (end <= flash_e);
        if (in_flash) return true;
    }

    return false;
}

}  // namespace SyscallValidator


// 供 PendSV 汇编读取的两个全局 TCB 指针
// 声明为非 volatile：汇编直接使用符号地址，编译器临界区内通过 Arch:: 保护
extern "C" {
    TaskControlBlock* volatile g_current_tcb_ptr = nullptr;
    TaskControlBlock* volatile g_next_tcb_ptr    = nullptr;
    volatile uint32_t g_switch_start_cycle = 0;

    // 由 PendSV_Handler 调用的 MPU 动态沙盒切换
    // RISC-V 不使用 ARM MPU 路径: PMP 入口由 trap.cpp 在任务切换时统一管理
    void mpu_switch_sandbox(TaskControlBlock* next) {
#if !defined(ARCH_RISCV32)
        if (next && next->size_pow2 > 0) {
            MPU::instance().update_user_sandbox_verified(next->mpu_sandbox);
        }
#else
        (void)next; // RISC-V: PMP 区域在 trap_handler_c 的上下文切换路径中更新
#endif
    }

    void pendsv_metrics_hook() {
        if (g_switch_start_cycle > 0) {
            Metrics::record(METRIC_CTX_SWITCH, Arch::get_cycle() - g_switch_start_cycle);
            g_switch_start_cycle = 0;
        }
    }
}

// 系统 Tick 计数器（全局可见，供 lwIP OSAL 等读取系统时间）
volatile uint32_t tick_count = 0;

extern "C" {
    // ================================================================
    // SVC 分发处理函数（由 boot.S 中的 SVC_Handler 调用）
    // frame 是硬件自动压栈的寄存器快照，通过它读取系统调用参数
    // ================================================================
    void SVC_Handler_C(InterruptFrame* frame) {
        isr_enter_cycle = Arch::get_cycle();
#if defined(ARCH_RISCV32)
        const uint8_t svc_number = static_cast<uint8_t>(frame->svc_num);
#else
        // 通过 PC 回溯到 SVC 指令，提取 8 位系统调用号
        const uint16_t svc_instr = reinterpret_cast<const uint16_t*>(frame->pc)[-1];
        const uint8_t  svc_number = static_cast<uint8_t>(svc_instr & 0xFF);
#endif

        // 获取当前任务的栈边界，用于参数指针校验
        const TaskControlBlock* cur = Scheduler::instance().get_current_tcb();
        const uintptr_t stack_base  = cur ? static_cast<uintptr_t>(cur->stack_base) : 0u;
        const size_t    stack_size  =
            cur ? (static_cast<size_t>(1) << cur->size_pow2) : 0u;

        switch (svc_number) {
            case SYS_PRINT: { // SysCall: 串口输出（在内核特权态安全调用）
                // 【参数校验】：arg0 指针必须属于该任务的栈或 Flash 只读段
                constexpr size_t MAX_PRINT_LEN = 256u;
                const char* str = reinterpret_cast<const char*>(frame->arg0);
                
                // 为了防止没有 \0 导致的越界读取，我们先验证最多 MAX_PRINT_LEN 字节是否在合法空间
                // 然后在安全范围内寻找 \0
                size_t actual_len = 0;
                bool safe = false;
                for (size_t i = 0; i < MAX_PRINT_LEN; i++) {
                    if (SyscallValidator::validate_user_ptr(str + i, 1, stack_base, stack_size)) {
                        if (str[i] == '\0') {
                            actual_len = i;
                            safe = true;
                            break;
                        }
                    } else {
                        break; // 越界
                    }
                }
                
                if (!safe) {
                    uart_puts("[Kernel] SYS_PRINT: invalid ptr or no null terminator rejected\n");
                    break;
                }
                uart_puts(str); // 此时字符串已知安全且有 \0
                break;
            }
            case SYS_YIELD: // SysCall: 任务 Yield
                Scheduler::instance().schedule();
                break;
            case SYS_SLEEP: { // SysCall: 任务 Sleep
                // 【参数校验】：sleep 时长不超过 10 分钟（600,000 ms）
                constexpr uint32_t MAX_SLEEP_MS = 600'000u;
                const uint32_t ms = frame->arg0;
                if (ms > MAX_SLEEP_MS) {
                    uart_puts("[Kernel] SYS_SLEEP: duration out of range\n");
                    break;
                }
                Scheduler::instance().sleep_ms(ms);
                break;
            }
            case SYS_CAP_DERIVE: { // SysCall: 派生 Capability (带权限降级)
                if (!cur) break;
                uint32_t src = frame->arg0;
                uint32_t dst = frame->arg1;
                uint32_t rights = frame->arg2; // bitmask: read(1), write(2), grant(4)
                
                if (src >= auroraos::kernel::MAX_CSPACE_SLOTS || dst >= auroraos::kernel::MAX_CSPACE_SLOTS) {
                    uart_puts("[Kernel] SYS_CAP_DERIVE: slot out of range\n");
                    break;
                }
                const auto& src_cap = cur->cspace[src];
                if (src_cap.type == auroraos::kernel::CapType::Null) {
                    uart_puts("[Kernel] SYS_CAP_DERIVE: source is null\n");
                    break;
                }
                
                bool req_r = rights & 1;
                bool req_w = rights & 2;
                bool req_g = rights & 4;
                
                // Privilege escalation check
                if ((req_r && !src_cap.rights.read) || 
                    (req_w && !src_cap.rights.write) || 
                    (req_g && !src_cap.rights.grant)) {
                    uart_puts("[Kernel] SYS_CAP_DERIVE: privilege escalation attempt. Terminating task.\n");
                    Scheduler::instance().set_task_state(cur->id, TaskState::Terminated);
                    Scheduler::instance().schedule();
                    break;
                }
                
                TaskControlBlock* mutable_cur = Scheduler::instance().get_current_tcb();
                auto& dst_cap = mutable_cur->cspace[dst];
                dst_cap.type = src_cap.type;
                dst_cap.object = src_cap.object;
                dst_cap.rights.read = req_r;
                dst_cap.rights.write = req_w;
                dst_cap.rights.grant = req_g;
                dst_cap.badge = src_cap.badge; // inherit badge
                break;
            }
            case SYS_CAP_MINT: { // SysCall: 派生 Capability 并颁发新 Badge
                if (!cur) break;
                uint32_t src = frame->arg0;
                uint32_t dst = frame->arg1;
                uint32_t rights = frame->arg2; // bitmask: read(1), write(2), grant(4)
                uint32_t badge = frame->arg3;
                
                if (src >= auroraos::kernel::MAX_CSPACE_SLOTS || dst >= auroraos::kernel::MAX_CSPACE_SLOTS) {
                    uart_puts("[Kernel] SYS_CAP_MINT: slot out of range\n");
                    break;
                }
                const auto& src_cap = cur->cspace[src];
                if (src_cap.type == auroraos::kernel::CapType::Null) {
                    uart_puts("[Kernel] SYS_CAP_MINT: source is null\n");
                    break;
                }
                
                bool req_r = rights & 1;
                bool req_w = rights & 2;
                bool req_g = rights & 4;
                
                // Privilege escalation check
                if ((req_r && !src_cap.rights.read) || 
                    (req_w && !src_cap.rights.write) || 
                    (req_g && !src_cap.rights.grant)) {
                    uart_puts("[Kernel] SYS_CAP_MINT: privilege escalation attempt. Terminating task.\n");
                    Scheduler::instance().set_task_state(cur->id, TaskState::Terminated);
                    Scheduler::instance().schedule();
                    break;
                }
                
                TaskControlBlock* mutable_cur = Scheduler::instance().get_current_tcb();
                auto& dst_cap = mutable_cur->cspace[dst];
                dst_cap.type = src_cap.type;
                dst_cap.object = src_cap.object;
                dst_cap.rights.read = req_r;
                dst_cap.rights.write = req_w;
                dst_cap.rights.grant = req_g;
                dst_cap.badge = badge; // mint new badge
                break;
            }
            case SYS_CAP_DELETE: { // SysCall: 删除 Capability
                if (!cur) break;
                uint32_t slot = frame->arg0;
                if (slot >= auroraos::kernel::MAX_CSPACE_SLOTS) {
                    uart_puts("[Kernel] SYS_CAP_DELETE: slot out of range\n");
                    break;
                }
                TaskControlBlock* mutable_cur = Scheduler::instance().get_current_tcb();
                mutable_cur->cspace[slot].type = auroraos::kernel::CapType::Null;
                mutable_cur->cspace[slot].object = nullptr;
                break;
            }
            case SYS_KILL: { // SysCall: 发送信号
                uint32_t target_id = frame->arg0;
                int sig = static_cast<int>(frame->arg1);
                
                if (sig < 1 || sig >= 16) {
                    frame->arg0 = static_cast<uint32_t>(-1);
                    break;
                }
                
                TaskControlBlock* target = Scheduler::instance().get_task_by_id(target_id);
                if (!target) {
                    frame->arg0 = static_cast<uint32_t>(-1);
                    break;
                }
                
                // Add to signal queue
                if (target->sig_count < TaskControlBlock::MAX_QUEUED_SIGNALS) {
                    target->signal_queue[target->sig_tail] = sig;
                    target->sig_tail = (target->sig_tail + 1) % TaskControlBlock::MAX_QUEUED_SIGNALS;
                    target->sig_count++;
                } else {
                    frame->arg0 = static_cast<uint32_t>(-1); // queue full
                    break;
                }
                
                // Wake up if necessary
                if (sig == SIGKILL) {
                    if (target->state != TaskState::Ready && target->state != TaskState::Running) {
                        Scheduler::instance().set_task_state(target->id, TaskState::Ready);
                    }
                } else if (!sigismember(&target->signal_mask, sig)) {
                    if (target->state == TaskState::Sleeping || target->state == TaskState::Blocked_On_Notify) {
                        Scheduler::instance().set_task_state(target->id, TaskState::Ready);
                    }
                }
                
                frame->arg0 = 0; // return success
                Scheduler::instance().schedule();
                break;
            }
            case SYS_SIGACTION: { // SysCall: 设置信号处理行为
                if (!cur) break;
                int sig = static_cast<int>(frame->arg0);
                const sigaction* act = reinterpret_cast<const sigaction*>(frame->arg1);
                sigaction* oldact = reinterpret_cast<sigaction*>(frame->arg2);
                
                if (sig < 1 || sig >= 16 || sig == SIGKILL) {
                    frame->arg0 = static_cast<uint32_t>(-1);
                    break;
                }
                
                TaskControlBlock* mutable_cur = Scheduler::instance().get_current_tcb();
                
                if (oldact) {
                    if (SyscallValidator::validate_user_ptr(oldact, sizeof(sigaction), stack_base, stack_size)) {
                        *oldact = mutable_cur->sig_actions[sig];
                    } else {
                        frame->arg0 = static_cast<uint32_t>(-1);
                        break;
                    }
                }
                
                if (act) {
                    if (SyscallValidator::validate_user_ptr(act, sizeof(sigaction), stack_base, stack_size)) {
                        mutable_cur->sig_actions[sig] = *act;
                    } else {
                        frame->arg0 = static_cast<uint32_t>(-1);
                        break;
                    }
                }
                
                frame->arg0 = 0;
                break;
            }
            case SYS_SIGPROCMASK: { // SysCall: 修改信号屏蔽字
                if (!cur) break;
                int how = static_cast<int>(frame->arg0);
                const uint32_t* set = reinterpret_cast<const uint32_t*>(frame->arg1);
                uint32_t* oldset = reinterpret_cast<uint32_t*>(frame->arg2);
                
                TaskControlBlock* mutable_cur = Scheduler::instance().get_current_tcb();
                
                if (oldset) {
                    if (SyscallValidator::validate_user_ptr(oldset, sizeof(uint32_t), stack_base, stack_size)) {
                        *oldset = mutable_cur->signal_mask;
                    } else {
                        frame->arg0 = static_cast<uint32_t>(-1);
                        break;
                    }
                }
                
                if (set) {
                    if (SyscallValidator::validate_user_ptr(set, sizeof(uint32_t), stack_base, stack_size)) {
                        uint32_t new_mask = *set;
                        // SIGKILL cannot be blocked
                        sigdelset(&new_mask, SIGKILL);
                        
                        if (how == SIG_BLOCK) {
                            mutable_cur->signal_mask |= new_mask;
                        } else if (how == SIG_UNBLOCK) {
                            mutable_cur->signal_mask &= ~new_mask;
                        } else if (how == SIG_SETMASK) {
                            mutable_cur->signal_mask = new_mask;
                        } else {
                            frame->arg0 = static_cast<uint32_t>(-1);
                            break;
                        }
                    } else {
                        frame->arg0 = static_cast<uint32_t>(-1);
                        break;
                    }
                }
                
                frame->arg0 = 0;
                break;
            }
            case SYS_IPC_CALL: { // SysCall: 发起 IPC 请求并阻塞等待响应
                if (!cur) break;
                uint32_t cap_id = frame->arg0;
                void* msg = reinterpret_cast<void*>(frame->arg1);
                uint32_t len = frame->arg2;
                
                // 从 arg3 解析 IpcReplyDesc
                const IpcReplyDesc* desc = reinterpret_cast<const IpcReplyDesc*>(frame->arg3);
                if (!SyscallValidator::validate_user_ptr(desc, sizeof(IpcReplyDesc), stack_base, stack_size)) {
                    uart_puts("[Kernel] SYS_IPC_CALL: invalid desc ptr\n");
                    break;
                }
                void* reply_buf = desc->buf;
                uint32_t max_reply_len = desc->max_len;
                
                // 校验 msg 和 reply_buf
                if (len > 0 && !SyscallValidator::validate_user_ptr(msg, len, stack_base, stack_size)) {
                    uart_puts("[Kernel] SYS_IPC_CALL: invalid msg ptr\n");
                    break;
                }
                if (max_reply_len > 0 && !SyscallValidator::validate_user_ptr(reply_buf, max_reply_len, stack_base, stack_size)) {
                    uart_puts("[Kernel] SYS_IPC_CALL: invalid reply_buf ptr\n");
                    break;
                }
                
                if (cap_id >= auroraos::kernel::MAX_CSPACE_SLOTS) {
                    uart_puts("[Kernel] SYS_IPC_CALL: cap_id out of range. Terminating task.\n");
                    Scheduler::instance().set_task_state(cur->id, TaskState::Terminated);
                    Scheduler::instance().schedule();
                    break;
                }

                const auto& cap = cur->cspace[cap_id];
                if (cap.type != auroraos::kernel::CapType::Endpoint || !cap.rights.write) {
                    uart_puts("[Kernel] SYS_IPC_CALL: capability check failed. Terminating task.\n");
                    Scheduler::instance().set_task_state(cur->id, TaskState::Terminated);
                    Scheduler::instance().schedule();
                    break;
                }

                auto* ep = static_cast<auroraos::kernel::Endpoint*>(cap.object);
                TaskControlBlock* mutable_cur = Scheduler::instance().get_current_tcb();
                ep->call(mutable_cur, msg, len, reply_buf, max_reply_len);
                Scheduler::instance().schedule(); // Block and switch task
                break;
            }
            case SYS_IPC_RECEIVE: { // SysCall: 接收 IPC 请求
                if (!cur) break;
                uint32_t cap_id = frame->arg0;
                void* msg_buf = reinterpret_cast<void*>(frame->arg1);
                uint32_t max_len = frame->arg2;
                uint32_t* out_sender_id = reinterpret_cast<uint32_t*>(frame->arg3);
                
                // 校验 msg_buf 和 out_sender_id
                if (max_len > 0 && !SyscallValidator::validate_user_ptr(msg_buf, max_len, stack_base, stack_size)) {
                    uart_puts("[Kernel] SYS_IPC_RECEIVE: invalid msg_buf ptr\n");
                    break;
                }
                if (out_sender_id && !SyscallValidator::validate_user_ptr(out_sender_id, sizeof(uint32_t), stack_base, stack_size)) {
                    uart_puts("[Kernel] SYS_IPC_RECEIVE: invalid out_sender_id ptr\n");
                    break;
                }
                
                if (cap_id >= auroraos::kernel::MAX_CSPACE_SLOTS) {
                    uart_puts("[Kernel] SYS_IPC_RECEIVE: cap_id out of range. Terminating task.\n");
                    Scheduler::instance().set_task_state(cur->id, TaskState::Terminated);
                    Scheduler::instance().schedule();
                    break;
                }

                const auto& cap = cur->cspace[cap_id];
                if (cap.type != auroraos::kernel::CapType::Endpoint || !cap.rights.read) {
                    uart_puts("[Kernel] SYS_IPC_RECEIVE: capability check failed. Terminating task.\n");
                    Scheduler::instance().set_task_state(cur->id, TaskState::Terminated);
                    Scheduler::instance().schedule();
                    break;
                }

                auto* ep = static_cast<auroraos::kernel::Endpoint*>(cap.object);
                TaskControlBlock* mutable_cur = Scheduler::instance().get_current_tcb();
                ep->receive(mutable_cur, msg_buf, max_len);
                
                if (mutable_cur->ipc_state == auroraos::kernel::IpcState::Receiving) {
                    // No sender was waiting, we blocked.
                    Scheduler::instance().schedule();
                } else if (out_sender_id) {
                    // We fast-pathed and received a message immediately.
                    *out_sender_id = mutable_cur->ipc_sender_id;
                }
                break;
            }
            case SYS_IPC_REPLY: { // SysCall: 回复 IPC 请求
                if (!cur) break;
                uint32_t sender_id = frame->arg0;
                void* reply_msg = reinterpret_cast<void*>(frame->arg1);
                uint32_t len = frame->arg2;
                
                // 校验 reply_msg
                if (len > 0 && !SyscallValidator::validate_user_ptr(reply_msg, len, stack_base, stack_size)) {
                    uart_puts("[Kernel] SYS_IPC_REPLY: invalid reply_msg ptr\n");
                    break;
                }
                
                TaskControlBlock* mutable_cur = Scheduler::instance().get_current_tcb();
                auroraos::kernel::Endpoint::reply(mutable_cur, sender_id, reply_msg, len);
                Scheduler::instance().schedule(); // Yield to the awoken sender (Fastpath)
                break;
            }
            default:
                uart_puts("[Kernel] Unknown SVC — possible exploit attempt\n");
                // 终止违规任务而非整个系统
                if (cur) {
                    Scheduler::instance().set_task_state(
                        cur->id, TaskState::Terminated);
                    Scheduler::instance().schedule();
                }
                break;
        }
    }

    // ================================================================
    // 内存管理异常处理（捕捉 MPU 违规访问）
    // ================================================================
    void MemManage_Handler(void) {
        uart_puts("\r\n[MemManage_Handler] Memory Protection Violation Detected! \r\n");
        uart_puts("Access Denied! Offending thread terminated by kernel.\r\n");
        
        // 【系统审查修复】：不要挂起整个系统，直接销毁违规线程，并将 CPU 让给其它存活任务
        TaskControlBlock* current = Scheduler::instance().get_current_tcb();
        if (current) {
            current->state = TaskState::Terminated; // 或者直接发送 SIGKILL
        }
        
        // 强制触发一次调度，让出 CPU
        Scheduler::instance().schedule();
        
        // MemManage_Handler 必须正常返回，以便硬件执行尾链（tail-chaining）并触发 PendSV
        return;
    }

    void HardFault_Handler(void) {
        uart_puts("\r\n[HardFault_Handler] Hard Fault Detected! System Halted.\r\n");
        while (1) {}
    }
}

// ================================================================
// SysTick 中断：系统心跳，驱动两件事：
//   1. tick_update()  — 将到期的休眠任务唤醒（设为 Ready）
//   2. schedule()     — 每 10ms 触发一次调度：
//                       * 高优先级任务唤醒后立即抢占低优先级
//                       * 同级任务轮转时间片
// ================================================================

#include "frame_scheduler.hpp"

extern "C" bool frame_scheduler_is_task_allowed(uint8_t priority) {
    return FrameSchedulerV2::instance().is_task_allowed(priority);
}

void SysTick_Handler(void) {
    isr_enter_cycle = Arch::get_cycle();
    tick_count++;
    
    // 1. 驱动软件定时器引擎
    TimerManager::instance().on_tick();

    // 2. 【核心注入】驱动蓝河帧感知时钟窗 (计算 33ms 边界)
    FrameSchedulerV2::instance().on_tick();

    Scheduler& sched = Scheduler::instance();
    sched.tick_update();
    
    // 每 5ms 触发一次高频时间片重新评估，保障 30fps 窗口内的微秒级响应
    if (tick_count % 5 == 0) {
        sched.schedule(); 
    }
}
