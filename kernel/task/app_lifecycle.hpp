#ifndef AURORA_APP_LIFECYCLE_HPP
#define AURORA_APP_LIFECYCLE_HPP

#include "task.hpp"
#include "../core/mutex.hpp"

enum class AppState {
    NOT_RUNNING,
    FOREGROUND, // 最高优先级，UI 实时刷新
    BACKGROUND, // 较低优先级，仅处理数据同步
    SUSPENDED   // 彻底停止调度，释放资源
};

struct AppControlBlock {
    uint32_t tid;
    AppState state;
    const char* app_name;

    void transition_to(AppState new_state) {
        // 整个迁移（校验 + 优先级/状态改写 + 锁释放）必须对 SysTick/ISR 原子：
        // 否则中断可在锁释放与状态迁移之间观察到半更新的 TCB。IrqGuard 可嵌套，
        // 与 Scheduler 内部的 IrqGuard 叠加安全。
        IrqGuard guard;
        if (state == new_state)
            return; // 同态迁移无意义，避免就绪队列无谓重排

        TaskControlBlock* tcb = Scheduler::instance().get_task_by_id(tid);
        // TCB 未分配（create_task 尚未调用）或已终止时，调度器侧的
        // set_task_priority/set_task_state 不会生效；此时若仍改写 state，
        // AppControlBlock 与 TCB 状态将不一致，故拒绝迁移。
        if (!tcb || tcb->scheduler.state == TaskState::Unallocated || tcb->scheduler.state == TaskState::Terminated)
            return;

        state = new_state;
        switch (new_state) {
        case AppState::FOREGROUND:
            Scheduler::instance().set_task_priority(tid, TaskPriority::Realtime); // 提升至渲染特权级
            Scheduler::instance().set_task_state(tid, TaskState::Ready);
            break;
        case AppState::BACKGROUND:
            Scheduler::instance().set_task_priority(tid, TaskPriority::Low); // 降级为后台处理级
            break;
        case AppState::SUSPENDED: {
            // 强行永久挂起调度并释放持有的互斥锁：复用内核统一清理路径
            // （kernel/core/mutex.cpp），它负责唤醒等待者、恢复 PIP 优先级并
            // 把任务从其等待的互斥锁中摘除，避免手动遍历 held_mutexes 时
            // force_unlock 改链导致的 next 失效。
            kernel_release_task_mutexes(tid);
            // 注意：lfs_file_t 等资源的释放需要对接 FS 层，此处可增加回调或事件通知
            Scheduler::instance().set_task_state(tid, TaskState::Suspended);
            break;
        }
        default:
            break;
        }
    }
};

#endif
