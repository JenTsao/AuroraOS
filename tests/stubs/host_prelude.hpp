#ifndef AURORA_HOST_TEST_PRELUDE_HPP
#define AURORA_HOST_TEST_PRELUDE_HPP

// =============================================================================
// host_prelude.hpp — Force-included before every test translation unit.
//
// Injected via CMake:
//   target_compile_options(aurora_tests PRIVATE -include ".../host_prelude.hpp")
//
// Purpose: Pre-emptively undefine POSIX signal macros that conflict with
// kernel/task.hpp's constexpr declarations.
//
// Root cause: <gtest/gtest.h> → <memory> → <shared_ptr.h> → <gthr.h> →
//   <gthr-default.h> → <pthread.h> → <signal.h>, which defines SIGINT etc.
// as preprocessor macros.  kernel/task.hpp then tries:
//   constexpr int SIGINT = 2;
// The preprocessor expands SIGINT→2, yielding "constexpr int 2 = 2;" → error.
//
// By force-including this file FIRST (before gtest.h), the undefs below
// have no effect at this point.  After signal.h runs and defines the macros,
// we need them gone BEFORE task.hpp sees them.
//
// A better approach: use -include to run this AFTER gtest/gtest.h but before
// the project headers.  We achieve this by including it from each test file's
// preamble — or by force-including it as the LAST pre-include so it fires
// after all std/gtest headers.
//
// The actual undefs need to be right before task.hpp is included, which is
// done in the task_wrapper.hpp below.
// =============================================================================

// Force-include <signal.h> NOW so all signal macros are defined...
#include <signal.h>
// ...then immediately undefine the ones that conflict with task.hpp.
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
#ifdef sa_handler
#  undef sa_handler
#endif

#endif  // AURORA_HOST_TEST_PRELUDE_HPP
