#ifndef MUTEX_HPP
#define MUTEX_HPP

// =============================================================================
// mutex.hpp 鈥?Host-native STUB (overrides kernel/mutex.hpp in tests)
//
// The real kernel/mutex.hpp includes task.hpp + syscall.hpp and calls
// Scheduler::instance().sleep_ms(1) inside its spin loop 鈥?which requires a
// live scheduler with at least one task.  That invariant cannot hold in
// isolated unit tests.
//
// This stub replaces Mutex with a std::mutex-backed RAII wrapper and
// LockGuard with a std::lock_guard equivalent, preserving the exact public
// API so that kernel/memory.hpp compiles and behaves correctly.
// =============================================================================

#include <mutex>

// ---------------------------------------------------------------------------
// Mutex
// ---------------------------------------------------------------------------
class Mutex {
public:
    Mutex() noexcept = default;

    // Non-copyable, non-movable (same contract as real kernel Mutex).
    Mutex(const Mutex&)            = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock()   { mtx_.lock(); }
    void unlock() { mtx_.unlock(); }

private:
    std::mutex mtx_;
};

// ---------------------------------------------------------------------------
// LockGuard 鈥?CP.20: RAII, never plain lock()/unlock()
// ---------------------------------------------------------------------------
struct LockGuard {
    explicit LockGuard(Mutex& m) : m_(m) { m_.lock(); }
    ~LockGuard()                          { m_.unlock(); }

    LockGuard(const LockGuard&)            = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Mutex& m_;
};

#endif  // MUTEX_HPP
