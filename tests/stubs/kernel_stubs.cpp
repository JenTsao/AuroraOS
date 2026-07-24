// =============================================================================
// kernel_stubs.cpp — Linker stubs for kernel symbols undefined on host
//
// Provides definitions for symbols declared in kernel/task.hpp with external
// linkage that are normally implemented in firmware-specific .cpp files:
//  - g_current_tcb_ptr / g_next_tcb_ptr  (apps/kernel.cpp on target)
//  - frame_scheduler_is_task_allowed     (apps/kernel.cpp on target)
//
// We include host_prelude.hpp first to resolve the POSIX signal macro
// conflict before pulling in task.hpp.
// =============================================================================

// Force-include the signal macro undef before pulling in task.hpp.
// (The -include CMake flag handles this for test_*.cpp files;  here we
// must do it explicitly because this file is compiled with the same flags.)
#include <signal.h>
#ifdef SIGINT
#  undef SIGINT
#endif
#ifdef SIGKILL
#  undef SIGKILL
#endif
#ifdef SIGALRM
#  undef SIGALRM
#endif
#ifdef SIGUSR1
#  undef SIGUSR1
#endif

#include "task.hpp"  // Provides TaskControlBlock, Scheduler (resolved via stubs/ path)
#include "../../metrics/metrics.hpp"

// ---------------------------------------------------------------------------
// Global TCB volatile pointers (extern "C" declarations in task.hpp).
// Scheduler::schedule() writes these before calling trigger_context_switch();
// on the host trigger_context_switch() is a no-op stub, so these pointers
// are never dereferenced during unit tests.
// ---------------------------------------------------------------------------
extern "C" {

TaskControlBlock* volatile g_current_tcb_ptr = nullptr;
TaskControlBlock* volatile g_next_tcb_ptr    = nullptr;
volatile uint32_t g_switch_start_cycle = 0;

// ---------------------------------------------------------------------------
// frame_scheduler_is_task_allowed — C-linkage stub
//
// Returns true so Scheduler::schedule() never filters tasks based on
// frame budget during unit tests.
// ---------------------------------------------------------------------------
bool frame_scheduler_is_task_allowed(uint8_t /*priority*/) {
    return true;
}

}  // extern "C"

namespace Arch {
    void disable_systick() {}
    void enable_systick() {}
    void start_wakeup_timer(uint32_t /*ticks*/) {}
    uint32_t stop_wakeup_timer() { return 0; }
}

// Metrics stubs
void Metrics::init() {}
void Metrics::start_measurement() {}
void Metrics::stop_measurement() {}
bool Metrics::is_active() { return false; }
void Metrics::record(MetricId /*id*/, uint32_t /*value*/) {}
void Metrics::inc_net_drop() {}
void Metrics::inc_softbus_register() {}
void Metrics::inc_heap_defrag() {}

LatencyRecorder& Metrics::get_recorder(MetricId /*id*/) {
    static LatencyRecorder dummy;
    return dummy;
}
PowerProfiler& Metrics::get_power_profiler() {
    static PowerProfiler dummy;
    return dummy;
}

namespace auroraos {
namespace ble {
namespace HalBle {
    void init() {}
    void start_advertising(const char* /*device_name*/) {}
    void stop_advertising() {}
    void disconnect() {}
    void notify_characteristic(uint16_t /*svc_uuid*/, const uint8_t* /*data*/, size_t /*len*/) {}
}
}
}
uint32_t Metrics::get_net_drops() { return 0; }
uint32_t Metrics::get_softbus_registers() { return 0; }
uint32_t Metrics::get_heap_defrags() { return 0; }
namespace Arch {
    void (*g_arch_test_interrupt_hook)() = nullptr;
}

// Weak stub for watchdog_feed — no-op in host tests
void watchdog_feed(uint32_t /*task_priority*/) {}

#include <stdio.h>
extern "C" void sys_print(const char* str) {
    printf("%s", str);
}
