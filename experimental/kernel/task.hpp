#pragma once
// Minimal stub for experimental/kernel/ path — satisfies the relative
// #include "../kernel/task.hpp" in experimental/net/wifi_driver.cpp.
// The real task.hpp pulls in arch_api.hpp / mpu.hpp / cspace.hpp / ipc.hpp
// which are all ARM-specific.  wifi_driver.cpp only includes this for the
// commented-out sleep_ms call, so an empty stub is sufficient for host tests.
