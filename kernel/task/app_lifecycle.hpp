#ifndef AURORA_APP_LIFECYCLE_HPP
#define AURORA_APP_LIFECYCLE_HPP

#include "task.hpp"

enum class AppState {
    NOT_RUNNING,
    FOREGROUND,  // 最高优先级，UI 实时刷新
    BACKGROUND,  // 较低优先级，仅处理数据同步
    SUSPENDED    // 彻底停止调度，释放资源
};

struct AppControlBlock {
    uint32_t tid;
    AppState state;
    const char* app_name;

    void transition_to(AppState new_state) {
        state = new_state;
        TaskControlBlock* tcb = Scheduler::instance().get_task_by_id(tid);
        if (!tcb) return;

        switch (new_state) {
            case AppState::FOREGROUND:
                Scheduler::instance().set_task_priority(tid, TaskPriority::Realtime); // 提升至渲染特权级
                Scheduler::instance().set_task_state(tid, TaskState::Ready);
                break;
            case AppState::BACKGROUND:
                Scheduler::instance().set_task_priority(tid, TaskPriority::Low);  // 降级为后台处理级
                break;
            case AppState::SUSPENDED: {
                // 强行永久挂起调度并释放持有的互斥锁
                Mutex* m = static_cast<Mutex*>(tcb->held_mutexes);
                while (m) {
                    Mutex* next = m->get_next_held();
                    m->force_unlock(tcb);
                    m = next;
                }
                tcb->held_mutexes = nullptr;
                // 注意：lfs_file_t 等资源的释放需要对接 FS 层，此处可增加回调或事件通知
                Scheduler::instance().set_task_state(tid, TaskState::Suspended); 
                break;
            }
            default: break;
        }
    }
};

#endif
