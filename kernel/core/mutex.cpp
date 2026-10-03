// =============================================================================
// kernel/core/mutex.cpp
//
// Mutex 子系统与调度器的解耦点：任务终止安全清理。
// Scheduler（task.hpp）只前向声明 Mutex，无法直接操作锁对象；
// 通过 task.hpp 声明的 kernel_release_task_mutexes() 弱符号约定，
// 由本文件在链接 Mutex 子系统时提供强实现。
//
// 由 Scheduler::terminate_task() / Scheduler::free_task() 调用：
//   1. 强制释放目标任务持有的所有互斥锁（唤醒等待者并恢复 PIP 优先级），
//      否则锁永久锁死，且 TCB 槽位复用后 owner 悬垂引发权限混乱。
//   2. 将目标任务从其正在等待的互斥锁等待掩码中摘除，避免 PIP 记账残留。
// =============================================================================
#include "mutex.hpp"
#include "task.hpp"

extern "C" void kernel_release_task_mutexes(uint32_t task_id) {
    TaskControlBlock* tcb = Scheduler::instance().get_task_by_id(task_id);
    if (!tcb)
        return;

    // 1. 释放持有的锁：force_unlock 会把自身从 owner 的 held_mutexes
    //    链表中摘除，因此循环到链表为空即可。步数上限仅为防御
    //    异常状态下的死循环（正常链长 = 任务同时持有的锁数）。
    int steps = 0;
    while (tcb->scheduler.held_mutexes != nullptr && steps < 64) {
        Mutex* m = static_cast<Mutex*>(tcb->scheduler.held_mutexes);
        m->force_unlock(tcb);
        ++steps;
    }

    // 2. 从等待队列摘除
    Mutex* waiting = tcb->scheduler.waiting_on_mutex;
    if (waiting != nullptr) {
        waiting->abandon_waiter(tcb);
        tcb->scheduler.waiting_on_mutex = nullptr;
    }
}
