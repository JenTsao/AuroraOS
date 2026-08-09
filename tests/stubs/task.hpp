#ifndef AURORA_TESTS_STUBS_TASK_HPP
#define AURORA_TESTS_STUBS_TASK_HPP

// =============================================================================
// task.hpp — Wrapper stub that patches POSIX signal macro conflicts
//
// Problem: kernel/task.hpp declares:
//   constexpr int SIGINT  = 2;
//   constexpr int SIGKILL = 9;
//   constexpr int SIGALRM = 14;
//   constexpr int SIGUSR1 = 10;
//
// On MinGW/Linux, <signal.h> (pulled in transitively by <pthread.h> through
// GoogleTest's <memory> include) defines these as preprocessor macros.
// When task.hpp sees "#define SIGINT 2" already in effect, then tries to
// declare "constexpr int SIGINT = 2;", the compiler expands SIGINT to 2,
// producing the nonsensical "constexpr int 2 = 2;" → "expected unqualified-id
// before numeric constant".
//
// Fix: #undef the signal macros immediately before including the real task.hpp.
// This stub lives in tests/stubs/ which is -I'd BEFORE kernel/, so it is
// found first when any test translation unit does #include "task.hpp".
// =============================================================================

// Suppress the POSIX signal macros that conflict with task.hpp's constexpr names.
// These macros are defined by <signal.h> via POSIX headers; they have the same
// numeric values as the constexpr declarations so no semantics are lost.
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

// Now include the real kernel/task.hpp.
// Use a relative path that skips stubs/ (we are in stubs/ already) and goes
// directly to kernel/task.hpp via the second entry in the include search path.
#include_next "task.hpp"

#endif  // AURORA_TESTS_STUBS_TASK_HPP
