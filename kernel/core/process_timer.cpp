#include "process_timer.hpp"
#include "../interrupt/timer.hpp"

namespace auroraos::kernel {

static inline uint32_t ms_to_ticks(uint32_t ms) {
    if (ms == 0) return 0;
    // 饱和处理：用户可传入任意 32 位毫秒值，必须防止 ms * TICK_RATE_HZ 回绕
    constexpr uint32_t kMaxMs = (UINT32_MAX - 999u) / Scheduler::TICK_RATE_HZ;
    if (ms > kMaxMs) {
        return UINT32_MAX;
    }
    uint32_t ticks = (ms * Scheduler::TICK_RATE_HZ + 999) / 1000;
    return (ticks == 0) ? 1 : ticks;
}

static inline uint32_t ticks_to_ms(uint32_t ticks) {
    // 饱和处理：防止 ticks * 1000 回绕（ms_to_ticks 饱和后 rem_ticks 可能很大）
    if (ticks > UINT32_MAX / 1000u) {
        return UINT32_MAX;
    }
    return (ticks * 1000) / Scheduler::TICK_RATE_HZ;
}

static ProcessTimerManager s_mgr_instance;

ProcessTimerManager& ProcessTimerManager::instance() {
    return s_mgr_instance;
}

// 撤销所有指向该定时器的权能（含派生与跨任务授予的副本）。
// 必须在清除 owner_task_id / allocated 之前调用，否则槽位复用后
// 旧权能持有者将能操作新属主的定时器（权能混淆）。
static void revoke_timer_caps(TaskControlBlock* owner, ProcessTimer* timer) {
    if (!owner || !timer) {
        return;
    }
    for (uint32_t s = 0; s < static_cast<uint32_t>(MAX_CSPACE_SLOTS); ++s) {
        Capability* c = CSpace::cap_lookup(owner, s);
        if (c && c->type == CapType::Timer && c->object == timer) {
            // cap_revoke 使所有任务中指向同一对象的权能失效；
            // cap_delete 移除源槽位本身。
            CSpace::cap_revoke(owner, s);
            CSpace::cap_delete(owner, s);
        }
    }
}

void ProcessTimerManager::init() {
    IrqGuard guard;
    for (size_t i = 0; i < MAX_TIMERS; i++) {
        timers_[i].allocated = false;
        timers_[i].active = false;
        timers_[i].owner_task_id = 0;
        timers_[i].timer_id = static_cast<uint32_t>(i);
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
            timers_[i].timer_id = static_cast<uint32_t>(i);
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

    // 先撤销全部指向该定时器的权能，再释放槽位，防止陈旧权能复用
    revoke_timer_caps(owner, &timers_[timer_id]);

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

int ProcessTimerManager::create_timer_cap(TaskControlBlock* owner, const ProcessTimerDesc* desc, int dst_slot) {
    if (!owner || !desc) {
        return -1;
    }

    int timer_id = create_timer(owner, desc);
    if (timer_id < 0) {
        return timer_id;
    }

    int slot = dst_slot;
    if (slot < 0) {
        slot = CSpace::cap_alloc_slot(owner);
        if (slot < 0) {
            delete_timer(owner, static_cast<uint32_t>(timer_id));
            return -2; // CSpace full
        }
    }

    Capability cap;
    cap.type = CapType::Timer;
    cap.rights = {true, true, true, 0}; // Read, Write, Grant
    cap.badge = static_cast<uint32_t>(timer_id);
    cap.object = &timers_[timer_id];

    if (!CSpace::cap_insert(owner, static_cast<uint32_t>(slot), cap)) {
        delete_timer(owner, static_cast<uint32_t>(timer_id));
        return -3;
    }

    return slot;
}

ProcessTimer* ProcessTimerManager::get_timer_by_cap(TaskControlBlock* owner, uint32_t cap_slot, uint32_t required_rights) {
    if (!owner) {
        return nullptr;
    }

    Capability* cap = CSpace::cap_lookup(owner, cap_slot);
    if (!cap || cap->type != CapType::Timer || !cap->object) {
        return nullptr;
    }

    if ((required_rights & CAP_RIGHT_READ) && !cap->rights.read) {
        return nullptr;
    }
    if ((required_rights & CAP_RIGHT_WRITE) && !cap->rights.write) {
        return nullptr;
    }

    ProcessTimer* timer = static_cast<ProcessTimer*>(cap->object);
    // 纵深防御：即使权能因某种路径未被撤销（如任务回收顺序异常），
    // 也不允许通过陈旧权能访问已释放或已改属主的定时器。
    if (!timer->allocated || timer->owner_task_id != owner->scheduler.id) {
        return nullptr;
    }
    return timer;
}

void ProcessTimerManager::delete_timer_by_ptr(ProcessTimer* timer) {
    if (!timer) {
        return;
    }

    IrqGuard guard;
    // 通过属主 task_id 定位 TCB 并撤销全部相关权能
    if (timer->owner_task_id != 0) {
        TaskControlBlock* owner = Scheduler::instance().get_task_by_id(timer->owner_task_id);
        if (owner) {
            revoke_timer_caps(owner, timer);
        }
    }
    timer->allocated = false;
    timer->active = false;
    timer->owner_task_id = 0;
    timer->flags = 0;
    timer->expire_tick = 0;
    timer->period_ticks = 0;
    timer->notify_param = 0;
}

void ProcessTimerManager::cleanup_task_timers(uint32_t task_id) {
    IrqGuard guard;
    // 任务终止回收：先撤销权能再释放定时器槽位
    TaskControlBlock* owner = Scheduler::instance().get_task_by_id(task_id);
    for (size_t i = 0; i < MAX_TIMERS; i++) {
        if (timers_[i].allocated && timers_[i].owner_task_id == task_id) {
            revoke_timer_caps(owner, &timers_[i]);
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
                        target->scheduler.state == TaskState::Sleeping ||
                        (target->scheduler.state == TaskState::Blocked_On_Notify &&
                         target->ipc.waiting_endpoint == nullptr)) {
                        // 仅唤醒阻塞在任务通知上的目标；阻塞在 Endpoint IPC
                        // 上的任务必须经 cancel_waiter 状态机迁移，直接唤醒
                        // 会产生幽灵队列条目。
                        Scheduler::instance().set_task_state(target->scheduler.id, TaskState::Ready);
                    }
                }
            } else if (flags & TimerFlags::NotifyEvent) {
                TaskControlBlock* target = Scheduler::instance().get_task_by_id(owner_id);
                if (target) {
                    target->ipc.notify_pending = true;
                    target->ipc.notify_value |= (param ? param : (1U << i));
                    if (target->scheduler.state == TaskState::Suspended ||
                        target->scheduler.state == TaskState::Sleeping ||
                        (target->scheduler.state == TaskState::Blocked_On_Notify &&
                         target->ipc.waiting_endpoint == nullptr)) {
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

extern "C" void kernel_cleanup_task_timers(uint32_t task_id) {
    ProcessTimerManager::instance().cleanup_task_timers(task_id);
}

} // namespace auroraos::kernel
