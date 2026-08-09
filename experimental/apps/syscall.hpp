#pragma once
// Stub for experimental/apps/ path — satisfies the relative
// #include "../apps/syscall.hpp" in experimental/net/wifi_driver.cpp.
// We forward-declare sys_print; the linker definition lives in
// tests/stubs/kernel_stubs.cpp (extern "C" void sys_print).
#ifdef __cplusplus
extern "C" {
#endif
void sys_print(const char* str);
#ifdef __cplusplus
}
#endif
