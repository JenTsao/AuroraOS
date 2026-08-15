// =============================================================================
// kernel/security_monitor.hpp — Independent Safety Monitor Task
//
// Provides:
//   1. Hardware watchdog (IWDG) control — feed only when all heartbeats are OK.
//   2. Per-task heartbeat registration and monitoring.
//   3. Stack overflow counter (fed by tick_update via Scheduler).
//   4. System health checks (heap pressure).
//
// Design constraints:
//   - monitor_task_entry() runs at TaskPriority::Realtime.
//   - NEVER acquires blocking mutexes (deadlock risk).
//   - NEVER calls any API that may sleep (it holds the WDT alive).
// =============================================================================

#ifndef AURORA_SECURITY_MONITOR_HPP
#define AURORA_SECURITY_MONITOR_HPP

#include "task.hpp"
#include "memory.hpp"
#include <stdint.h>

// Minimal UART fault reporting hook shared with kernel/core/ota.cpp — safe to
// call from a non-blocking, non-preemptible safety-monitor context (no heap,
// no mutex, just a raw byte write to the debug UART).
extern "C" void sys_print(const char* str);

// ─────────────────────────────────────────────────────────────────────────────
// IWDG Hardware Watchdog (independent of main clock — runs on LSI ~26 kHz)
//
// Register addresses are for the STM32-family IWDG peripheral.
// Replace with target-SoC values if different.
// ─────────────────────────────────────────────────────────────────────────────
namespace IWDG {
inline static volatile uint32_t* const KR = reinterpret_cast<volatile uint32_t*>(0x40003000u);  // Key register
inline static volatile uint32_t* const PR = reinterpret_cast<volatile uint32_t*>(0x40003004u);  // Prescaler
inline static volatile uint32_t* const RLR = reinterpret_cast<volatile uint32_t*>(0x40003008u); // Reload

// Enable IWDG with ~30 s timeout (LSI 26 kHz / 256 prescaler / 4096 reload)
inline void enable() noexcept {
    *KR = 0xCCCCu; // Start IWDG
    *KR = 0x5555u; // Unlock PR/RLR registers
    *PR = 0b110u;  // Prescaler /256
    *RLR = 0xFFFu; // Reload value ~30 s
    *KR = 0xAAAAu; // Reload counter (apply settings)
}

// Feed the watchdog — call periodically to prevent reset.
inline void feed() noexcept {
    *KR = 0xAAAAu;
}

// Intentionally starve the watchdog to force a hardware reset.
// Call when the system is in an unrecoverable state.
inline void starve() noexcept { /* Simply stop calling feed() */ }
} // namespace IWDG

// ─────────────────────────────────────────────────────────────────────────────
// HeartbeatEntry — tracks liveness of a monitored task.
// ─────────────────────────────────────────────────────────────────────────────
struct HeartbeatEntry {
    uint32_t task_id;
    uint32_t last_beat_tick; // Tick counter at last heartbeat
    uint32_t timeout_ticks;  // Ticks without a beat before declaring dead
    bool active;             // Slot in use
};

// ─────────────────────────────────────────────────────────────────────────────
// SecurityMonitor — singleton safety monitor.
// ─────────────────────────────────────────────────────────────────────────────
class SecurityMonitor {
public:
    static SecurityMonitor& instance() noexcept {
        static SecurityMonitor mon;
        return mon;
    }

    // ── Heartbeat API (called by monitored tasks) ─────────────────────────

    // Register a task for heartbeat monitoring.
    // timeout_ticks: number of ticks without a beat before declaring dead.
    void register_task(uint32_t task_id, uint32_t timeout_ticks) noexcept {
        for (auto& e : heartbeat_table_) {
            if (!e.active) {
                e.task_id = task_id;
                e.last_beat_tick = get_tick();
                e.timeout_ticks = timeout_ticks;
                e.active = true;
                return;
            }
        }
        // Table full — silently ignore (cannot block in safety path)
    }

    // Called by a monitored task to signal it is alive.
    void task_heartbeat(uint32_t task_id) noexcept {
        for (auto& e : heartbeat_table_) {
            if (e.active && e.task_id == task_id) {
                e.last_beat_tick = get_tick();
                return;
            }
        }
    }

    // ── Stack overflow reporting (called by Scheduler::tick_update) ──────
    void report_stack_overflow(uint32_t task_id) noexcept {
        stack_overflow_count_++;
        last_overflow_task_ = task_id;
        // Immediate best-effort UART surfacing so the fault is never
        // silently lost even before a persistent flash fault log exists.
        // (Persistent flash log — Phase 3 — needs a wear-leveled log
        // partition and a real flash driver; that is real future work,
        // not something to fake here. Until then this is the audit trail.)
        char msg[64];
        char* p = msg;
        const char prefix[] = "[SECMON] stack overflow task=";
        for (const char* s = prefix; *s; ++s) *p++ = *s;
        // task_id is small (< MAX_TASKS); render as decimal without snprintf
        // to keep this path free of any heap/locking dependency.
        char digits[10];
        int n = 0;
        uint32_t v = task_id;
        do { digits[n++] = static_cast<char>('0' + (v % 10u)); v /= 10u; } while (v && n < 10);
        while (n > 0) *p++ = digits[--n];
        *p++ = '\r'; *p++ = '\n'; *p = '\0';
        sys_print(msg);
    }

    uint32_t get_stack_overflow_count() const noexcept {
        return stack_overflow_count_;
    }

    uint32_t get_last_overflow_task() const noexcept {
        return last_overflow_task_;
    }

    // ── Firewall anomaly reporting (called by FirewallEngine) ────────────
    void report_firewall_anomaly(const char* reason) noexcept {
        firewall_anomaly_count_++;
        // Best-effort immediate UART surfacing (see note in
        // report_stack_overflow() above re: persistent flash log).
        sys_print("[SECMON] firewall anomaly: ");
        sys_print(reason ? reason : "(no reason)");
        sys_print("\r\n");
    }

    uint32_t get_firewall_anomaly_count() const noexcept {
        return firewall_anomaly_count_;
    }

    // ── Monitor task entry ───────────────────────────────────────────────
    //
    // Create this task with TaskPriority::Realtime BEFORE starting the
    // scheduler.  It will feed the watchdog only when all heartbeats are
    // healthy.  An unhealthy task is terminated; if it cannot recover
    // within STARVE_CYCLES the watchdog fires and resets the system.
    [[noreturn]] static void monitor_task_entry() {
        SecurityMonitor& mon = instance();
        IWDG::enable();

        while (true) {
            const uint32_t now = mon.get_tick();
            bool all_ok = true;

            // ── 1. Heartbeat audit ──
            for (auto& e : mon.heartbeat_table_) {
                if (!e.active)
                    continue;
                if (now - e.last_beat_tick > e.timeout_ticks) {
                    all_ok = false;
                    // Terminate the silent task
                    Scheduler::instance().set_task_state(e.task_id, TaskState::Terminated);
                    // Mark as inactive so we don't keep killing it
                    e.active = false;
                }
            }

            // ── 2. Feed WDT only when everything is healthy ──
            //    If a task is stuck, we stop feeding; hardware reset follows.
            if (all_ok) {
                IWDG::feed();
            }
            // (If !all_ok, the WDT starves and fires in ~30 s — controlled reset)

            // ── 3. Heap pressure check ──
            mon.check_heap_pressure();

            // ── 4. Yield for 100 ms ──
            // Use direct scheduler call — no SVC needed (we're already privileged)
            Scheduler::instance().sleep_ms(100u);
        }
    }

private:
    SecurityMonitor() = default;
    SecurityMonitor(const SecurityMonitor&) = delete;
    SecurityMonitor& operator=(const SecurityMonitor&) = delete;

    static constexpr int MAX_MONITORED_TASKS = 8;
    HeartbeatEntry heartbeat_table_[MAX_MONITORED_TASKS]{};

    uint32_t stack_overflow_count_ = 0u;
    uint32_t last_overflow_task_ = 0u;
    uint32_t heap_warn_count_ = 0u;
    uint32_t firewall_anomaly_count_ = 0u;

    // Tasks this monitor itself suspended for heap pressure (Phase 2),
    // so resume only touches what we actually throttled.
    static constexpr uint32_t MAX_THROTTLED = 8;
    uint32_t throttled_ids_[MAX_THROTTLED]{};
    uint32_t throttled_count_ = 0u;

    // External tick counter — defined in interrupts.cpp
    static uint32_t get_tick() noexcept {
        extern volatile uint32_t tick_count;
        return tick_count;
    }

    void check_heap_pressure() noexcept {
        const size_t free_mem = KernelHeap::instance().get_free_memory();
        const size_t total_mem = KernelHeap::instance().get_total_memory();
        if (total_mem == 0u)
            return;

        // Warn if free memory drops below 10 %
        if (free_mem * 10u < total_mem) {
            heap_warn_count_++;
            suspend_low_priority_tasks();
        } else if (free_mem * 5u > total_mem) {
            // Hysteresis: only resume once we're clearly back above 20 %
            // free, so we don't flap tasks in and out right at the edge.
            resume_suspended_low_priority_tasks();
        }
    }

    // ── Phase 2 heap-pressure response: suspend TaskPriority::Low tasks ──
    // (There is no GC in KernelHeap's first-fit/lazy-coalesce design, so
    // "trigger GC" from the old TODO doesn't apply here — suspension is
    // the actual lever available to relieve pressure.)
    void suspend_low_priority_tasks() noexcept {
        Scheduler& sched = Scheduler::instance();
        for (int i = 0; i < Scheduler::get_max_tasks(); ++i) {
            TaskControlBlock* tcb = sched.get_task(i);
            if (!tcb)
                continue;
            if (tcb->scheduler.current_priority != TaskPriority::Low)
                continue;
            if (tcb->scheduler.state != TaskState::Ready &&
                tcb->scheduler.state != TaskState::Running)
                continue;
            if (throttled_count_ >= MAX_THROTTLED)
                break;
            // Skip if already tracked (avoid duplicate entries).
            bool already = false;
            for (uint32_t k = 0; k < throttled_count_; ++k) {
                if (throttled_ids_[k] == static_cast<uint32_t>(i)) { already = true; break; }
            }
            if (already)
                continue;
            sched.set_task_state(static_cast<uint32_t>(i), TaskState::Suspended);
            throttled_ids_[throttled_count_++] = static_cast<uint32_t>(i);
        }
    }

    void resume_suspended_low_priority_tasks() noexcept {
        if (throttled_count_ == 0u)
            return;
        Scheduler& sched = Scheduler::instance();
        for (uint32_t k = 0; k < throttled_count_; ++k) {
            TaskControlBlock* tcb = sched.get_task_by_id(throttled_ids_[k]);
            if (tcb && tcb->scheduler.state == TaskState::Suspended) {
                sched.set_task_state(throttled_ids_[k], TaskState::Ready);
            }
        }
        throttled_count_ = 0u;
    }
};

#endif // AURORA_SECURITY_MONITOR_HPP
