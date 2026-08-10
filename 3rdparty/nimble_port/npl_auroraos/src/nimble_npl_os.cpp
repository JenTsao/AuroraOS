#include "nimble/nimble_npl_os.h"
#include "kernel/mutex.hpp"
#include "kernel/semaphore.hpp"
#include "kernel/msg_queue.hpp"
#include "kernel/timer.hpp"
#include "kernel/task.hpp"

extern "C" {

// ========================================================
// Mutex
// ========================================================
ble_npl_error_t ble_npl_mutex_init(struct ble_npl_mutex *mu) {
    mu->mu = new Mutex();
    return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_mutex_pend(struct ble_npl_mutex *mu, ble_npl_time_t timeout) {
    Mutex* m = static_cast<Mutex*>(mu->mu);
    bool success = m->lock(timeout == BLE_NPL_TIME_FOREVER ? 0xFFFFFFFF : timeout);
    return success ? BLE_NPL_OK : BLE_NPL_TIMEOUT;
}

ble_npl_error_t ble_npl_mutex_release(struct ble_npl_mutex *mu) {
    Mutex* m = static_cast<Mutex*>(mu->mu);
    m->unlock();
    return BLE_NPL_OK;
}

// ========================================================
// Semaphore
// ========================================================
ble_npl_error_t ble_npl_sem_init(struct ble_npl_sem *sem, uint16_t tokens) {
    Semaphore* s = new Semaphore(tokens);
    sem->sem = s;
    return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_sem_pend(struct ble_npl_sem *sem, ble_npl_time_t timeout) {
    Semaphore* s = static_cast<Semaphore*>(sem->sem);
    if (timeout == BLE_NPL_TIME_FOREVER) {
        s->wait();
        return BLE_NPL_OK;
    } else if (timeout == 0) {
        return s->try_wait() ? BLE_NPL_OK : BLE_NPL_TIMEOUT;
    } else {
        // Simple polling wait for finite timeout (AuroraOS Semaphore doesn't natively support timeout yet)
        uint32_t start = TimerManager::instance().get_current_tick();
        while (TimerManager::instance().get_current_tick() - start < timeout) {
            if (s->try_wait()) return BLE_NPL_OK;
            Task::sleep(1);
        }
        return BLE_NPL_TIMEOUT;
    }
}

ble_npl_error_t ble_npl_sem_release(struct ble_npl_sem *sem) {
    Semaphore* s = static_cast<Semaphore*>(sem->sem);
    s->signal();
    return BLE_NPL_OK;
}

uint16_t ble_npl_sem_get_count(struct ble_npl_sem *sem) {
    // Semaphore count is private in AuroraOS, dummy return 0 if not needed
    return 0; 
}

// ========================================================
// Time
// ========================================================
uint32_t ble_npl_time_get(void) {
    return TimerManager::instance().get_current_tick();
}

ble_npl_error_t ble_npl_time_ms_to_ticks(uint32_t ms, ble_npl_time_t *out_ticks) {
    *out_ticks = ms; // Assuming 1 tick = 1 ms in AuroraOS
    return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_time_ticks_to_ms(ble_npl_time_t ticks, uint32_t *out_ms) {
    *out_ms = ticks;
    return BLE_NPL_OK;
}

// ========================================================
// Hardware / OS
// ========================================================
bool ble_npl_os_started(void) {
    return true; // Assume kernel is running when NimBLE starts
}

void *ble_npl_get_current_task_id(void) {
    return Scheduler::instance().get_current_tcb();
}

void ble_npl_hw_enter_critical(void) {
    Arch::disable_interrupts();
}

void ble_npl_hw_exit_critical(void) {
    Arch::enable_interrupts();
}

// ========================================================
// Event Queue
// ========================================================
// We map ble_npl_eventq to a MessageQueue<struct ble_npl_event*, 32>
typedef MessageQueue<struct ble_npl_event*, 32> BleEventQ;

void ble_npl_eventq_init(struct ble_npl_eventq *evq) {
    BleEventQ* q = new BleEventQ();
    q->init();
    evq->q = q;
}

struct ble_npl_event *ble_npl_eventq_get(struct ble_npl_eventq *evq, ble_npl_time_t tmo) {
    BleEventQ* q = static_cast<BleEventQ*>(evq->q);
    struct ble_npl_event *ev = nullptr;
    
    if (tmo == BLE_NPL_TIME_FOREVER) {
        ev = q->pop();
    } else if (tmo == 0) {
        q->try_pop(ev);
    } else {
        uint32_t start = TimerManager::instance().get_current_tick();
        while (TimerManager::instance().get_current_tick() - start < tmo) {
            if (q->try_pop(ev)) break;
            Task::sleep(1);
        }
    }
    
    if (ev) ev->queued = false;
    return ev;
}

void ble_npl_eventq_put(struct ble_npl_eventq *evq, struct ble_npl_event *ev) {
    BleEventQ* q = static_cast<BleEventQ*>(evq->q);
    if (ev->queued) return;
    ev->queued = true;
    q->push(ev);
}

void ble_npl_eventq_remove(struct ble_npl_eventq *evq, struct ble_npl_event *ev) {
    ev->queued = false;
}

void ble_npl_event_run(struct ble_npl_event *ev) {
    if (ev->fn) ev->fn(ev);
}

// ========================================================
// Callout (Timer)
// ========================================================
void ble_npl_callout_init(struct ble_npl_callout *co, struct ble_npl_eventq *evq,
                          void (*ev_cb)(struct ble_npl_event *), void *ev_arg) {
    co->evq = evq;
    co->ev.fn = ev_cb;
    co->ev.arg = ev_arg;
    co->ev.queued = false;
    co->handle = nullptr; // Note: For a robust implementation, a Software Timer must be integrated here
}

ble_npl_error_t ble_npl_callout_reset(struct ble_npl_callout *co, ble_npl_time_t ticks) {
    // TODO: Implement using AuroraOS Timer Manager
    return BLE_NPL_OK;
}

void ble_npl_callout_stop(struct ble_npl_callout *co) {
    // TODO: Implement using AuroraOS Timer Manager
}

} // extern "C"
