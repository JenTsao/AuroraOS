#pragma once
// Stub for experimental/apps/ path.  The real syscall.hpp lives in syscall/ and
// is shadowed by tests/stubs/syscall.hpp.  This file exists only to satisfy the
// relative #include "../apps/syscall.hpp" in experimental/net/wifi_driver.cpp.
#include "../../syscall/syscall.hpp"
