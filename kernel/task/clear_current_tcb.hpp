#ifndef AURORA_CLEAR_CURRENT_TCB_HPP
#define AURORA_CLEAR_CURRENT_TCB_HPP

// Called from Scheduler::init() so re-init does not keep a stale
// g_current_tcb_ptr from a previous test or previous kernel epoch.
inline void aurora_clear_current_tcb_ptrs() {
    g_current_tcb_ptr = nullptr;
    g_next_tcb_ptr = nullptr;
}

#endif
