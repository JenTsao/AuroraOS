#include "process_timer.hpp"
#include "../interrupt/timer.hpp"

namespace auroraos::kernel {

static inline uint32_t ms_to_ticks(uint32_t ms) {
    if (ms == 0) return 0;
    uint32_t ticks = (ms * Scheduler::TICK_RATE_HZ + 999) / 1000;
    return (ticks == 0) ? 1 : ticks;
}

static inline uint32_t ticks_to_ms(uint32_t ticks) {
    return (ticks * 1000) / Scheduler::TICK_RATE_HZ;
}

ProcessTimerManager::ProcessTimerManager() {
    init();
}

ProcessTimerManager& ProcessTimerManager::instance() {
    static ProcessTimerManager s_mgr;
    return s_mgr;
}

void ProcessTimerManager::init() {
    IrqGuard guard;
    for (size_t i = 0; i < MAX_TIMERS; i++) {
        timers_[i].allocated = false;
        timers_[i].active = false;
        timers_[i].owner_task_id = 0;
        timers_[i].flags = 0;
        timers_[i].expire_tick = 0;
        timers_[i].period_ticks = 0;
        timers_[i].notify_param = 0;
    }
}

int ProcessTimerManager::create_timer(TaskControlBlock* owner, const ProcessTimerDesc* desc) {
    if (!owner || !desc) {
        return -1;
    }

    IrqGuard guard;
    for (size_t i = 0; i < MAX_TIMERS; i++) {
        if (!timers_[i].allocated) {
            timers_[i].allocated = true;
            timers_[i].owner_task_id = owner->scheduler.id;
            timers_[i].flags = desc->flags;
            timers_[i].notify_param = desc->notify_param;
            timers_[i].period_ticks = ms_to_ticks(desc->interval_ms);

            uint32_t cur_tick = TimerManager::instance().get_current_tick();
            if (desc->flags & TimerFlags::Absolute) {
                timers_[i].expire_tick = ms_to_ticks(desc->initial_delay_ms);
            } else {
                timers_[i].expire_tick = cur_tick + ms_to_ticks(desc->initial_delay_ms);
            }

            // 若初始延时 > 0 或指定了绝对时间，则自动激活
            if (desc->initial_delay_ms > 0 || (desc->flags & TimerFlags::Absolute)) {
                timers_[i].active = true;
            } else {
                timers_[i].active = false;
            }

            return static_cast<int>(i);
        }
    }
    return -2; // 槽位耗尽 (ENOSPC)
}

int ProcessTimerManager::start_timer(TaskControlBlock* owner, uint32_t timer_id, const ProcessTimerDesc* desc) {
    if (!owner || timer_id >= MAX_TIMERS) {
        return -1;
    }

    IrqGuard guard;
    if (!timers_[timer_id].allocated || timers_[timer_id].owner_task_id != owner->scheduler.id) {
        return -1; // 无权限或未分配
    }

    if (desc) {
        timers_[timer_id].flags = desc->flags;
        timers_[timer_id].notify_param = desc->notify_param;
        timers_[timer_id].period_ticks = ms_to_ticks(desc->interval_ms);

        uint32_t cur_tick = TimerManager::instance().get_current_tick();
        if (desc->flags & TimerFlags::Absolute) {
            timers_[timer_id].expire_tick = ms_to_ticks(desc->initial_delay_ms);
        } else {
            timers_[timer_id].expire_tick = cur_tick + ms_to_ticks(desc->initial_delay_ms);
        }
    }

    timers_[timer_id].active = true;
    return 0;
}

int ProcessTimerManager::stop_timer(TaskControlBlock* owner, uint32_t timer_id) {
    if (!owner || timer_id >= MAX_TIMERS) {
        return -1;
    }

    IrqGuard guard;
    if (!timers_[timer_id].allocated || timers_[timer_id].owner_task_id != owner->scheduler.id) {
        return -1;
    }

    timers_[timer_id].active = false;
    return 0;
}

int ProcessTimerManager::delete_timer(TaskControlBlock* owner, uint32_t timer_id) {
    if (!owner || timer_id >= MAX_TIMERS) {
        return -1;
    }

    IrqGuard guard;
    if (!timers_[timer_id].allocated || timers_[timer_id].owner_task_id != owner->scheduler.id) {
        return -1;
    }

    timers_[timer_id].allocated = false;
    timers_[timer_id].active = false;
    timers_[timer_id].owner_task_id = 0;
    timers_[timer_id].flags = 0;
    timers_[timer_id].expire_tick = 0;
    timers_[timer_id].period_ticks = 0;
    timers_[timer_id].notify_param = 0;
    return 0;
}

int ProcessTimerManager::get_time(TaskControlBlock* owner, uint32_t timer_id, uint32_t* out_remaining_ms) {
    if (!owner || timer_id >= MAX_TIMERS || !out_remaining_ms) {
        return -1;
    }

    IrqGuard guard;
    if (!timers_[timer_id].allocated || timers_[timer_id].owner_task_id != owner->scheduler.id) {
        return -1;
    }

    if (!timers_[timer_id].active) {
        *out_remaining_ms = 0;
        return 0;
    }

    uint32_t cur_tick = TimerManager::instance().get_current_tick();
    uint32_t rem_ticks = (timers_[timer_id].expire_tick > cur_tick)
                             ? (timers_[timer_id].expire_tick - cur_tick)
                             : 0;
    *out_remaining_ms = ticks_to_ms(rem_ticks);
    return 0;
}

void ProcessTimerManager::cleanup_task_timers(uint32_t task_id) {
    IrqGuard guard;
    for (size_t i = 0; i < MAX_TIMERS; i++) {
        if (timers_[i].allocated && timers_[i].owner_task_id == task_id) {
            timers_[i].allocated = false;
            timers_[i].active = false;
            timers_[i].owner_task_id = 0;
            timers_[i].flags = 0;
            timers_[i].expire_tick = 0;
            timers_[i].period_ticks = 0;
            timers_[i].notify_param = 0;
        }
    }
}

void ProcessTimerManager::on_tick() {
    uint32_t cur_tick = TimerManager::instance().get_current_tick();

    for (size_t i = 0; i < MAX_TIMERS; i++) {
        if (!timers_[i].allocated || !timers_[i].active) {
            continue;
        }

        if (cur_tick >= timers_[i].expire_tick) {
            uint32_t owner_id = timers_[i].owner_task_id;
            uint32_t flags = timers_[i].flags;
            uint32_t param = timers_[i].notify_param;

            // 1. 发送通知
            if (flags & TimerFlags::NotifySignal) {
                uint32_t signo = (param != 0) ? param : 14; // SIGALRM 默认为 14
                Scheduler::instance().send_signal(owner_id, signo);
            } else if (flags & TimerFlags::NotifyIpc) {
                TaskControlBlock* target = Scheduler::instance().get_task_by_id(owner_id);
                if (target) {
                    target->ipc.notify_pending = true;
                    target->ipc.notify_value |= (1U << (param & 31));
                    if (target->scheduler.state == TaskState::Suspended ||
                        target->scheduler.state == TaskState::Sleeping) {
                        Scheduler::instance().set_task_state(target->scheduler.id, TaskState::Ready);
                    }
                }
            } else if (flags & TimerFlags::NotifyEvent) {
                TaskControlBlock* target = Scheduler::instance().get_task_by_id(owner_id);
                if (target) {
                    target->ipc.notify_pending = true;
                    target->ipc.notify_value |= (param ? param : (1U << i));
                    if (target->scheduler.state == TaskState::Suspended ||
                        target->scheduler.state == TaskState::Sleeping) {
                        Scheduler::instance().set_task_state(target->scheduler.id, TaskState::Ready);
                    }
                }
            } else {
                // 默认行为：发送 SIGALRM
                Scheduler::instance().send_signal(owner_id, 14);
            }

            // 2. 周期重装或单次停止
            if ((flags & TimerFlags::Periodic) && timers_[i].period_ticks > 0) {
                timers_[i].expire_tick += timers_[i].period_ticks;
                // 防止由于系统长时间延迟导致过期时间严重滞后
                if (cur_tick >= timers_[i].expire_tick) {
                    timers_[i].expire_tick = cur_tick + timers_[i].period_ticks;
                }
            } else {
                timers_[i].active = false;
            }
        }
    }
}

uint32_t ProcessTimerManager::get_next_expire_ticks() const {
    uint32_t min_ticks = 0xFFFFFFFF;
    uint32_t cur_tick = TimerManager::instance().get_current_tick();

    for (size_t i = 0; i < MAX_TIMERS; i++) {
        if (timers_[i].allocated && timers_[i].active) {
            uint32_t remaining = (timers_[i].expire_tick > cur_tick)
                                     ? (timers_[i].expire_tick - cur_tick)
                                     : 0;
            if (remaining < min_ticks) {
                min_ticks = remaining;
            }
        }
    }
    return min_ticks;
}

void ProcessTimerManager::fast_forward_ticks(uint32_t skipped_ticks) {
    (void)skipped_ticks;
    on_tick();
}

void process_timer_on_tick() {
    ProcessTimerManager::instance().on_tick();
}

void process_timer_fast_forward(uint32_t ticks) {
    ProcessTimerManager::instance().fast_forward_ticks(ticks);
}

uint32_t process_timer_get_next_expire_ticks() {
    return ProcessTimerManager::instance().get_next_expire_ticks();
}

} // namespace auroraos::kernel
