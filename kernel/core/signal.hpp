#ifndef AURORA_SIGNAL_HPP
#define AURORA_SIGNAL_HPP

#include "../syscall/syscall.hpp"
#include "task.hpp"

// ========================================================
// POSIX 信号标准 API
// ========================================================

// 1. 注册当前线程的信号处理回调函数
inline SignalHandler signal(int sig, SignalHandler handler) {
    if (sig < 1 || sig >= 16) return nullptr;
    
    SignalAction act;
    act.sa_handler = handler;
    act.sa_mask = 0;
    act.sa_flags = SA_RESETHAND | SA_NODEFER; // Match original basic signal() behavior

    SignalAction oldact;
    if (sys_sigaction(sig, &act, &oldact) == 0) {
        return oldact.sa_handler;
    }
    return nullptr;
}

// 2. 向指定 ID 的任务发送信号
inline int kill(uint32_t target_task_id, int sig) {
    return sys_kill(target_task_id, sig);
}

// 3. 向自己发送信号
inline int raise(int sig) {
    TaskControlBlock* current = Scheduler::instance().get_current_tcb();
    return kill(current->id, sig);
}

#endif
