#ifndef AURORA_TESTS_MUTEX_STUB_HPP
#define AURORA_TESTS_MUTEX_STUB_HPP

// =============================================================================
// mutex_stub.hpp — Host-native stub for kernel/mutex.hpp
//
// Purpose: kernel/memory.hpp includes "mutex.hpp" and uses Mutex + LockGuard.
//          On real hardware these wrap SVC-based critical sections; on host
//          we back them with std::mutex so thread-safety semantics are
//          preserved even if tests were to run multi-threaded.
//
// This header is injected via -include or by placing tests/stubs/ first in
// the include path (overrides the real kernel/mutex.hpp for test builds).
// =============================================================================

#include <mutex>

// ---------------------------------------------------------------------------
// Mutex — wraps std::mutex for host builds
// ---------------------------------------------------------------------------
class Mutex {
public:
    Mutex() = default;

    // Non-copyable, non-movable — mirrors the real kernel Mutex contract.
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock()   noexcept { mtx_.lock(); }
    void unlock() noexcept { mtx_.unlock(); }

private:
    std::mutex mtx_;
};

// ---------------------------------------------------------------------------
// LockGuard — RAII wrapper, mirrors kernel/mutex.hpp interface
// ---------------------------------------------------------------------------
class LockGuard {
public:
    explicit LockGuard(Mutex& m) noexcept : mutex_(m) { mutex_.lock(); }
    ~LockGuard() noexcept { mutex_.unlock(); }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Mutex& mutex_;
};

#endif  // AURORA_TESTS_MUTEX_STUB_HPP
