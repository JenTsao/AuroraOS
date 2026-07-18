#include "symbol_export.hpp"
#include "posix.hpp"
#include "syscall.hpp"
#include "../apps/lua_ui_binding.hpp"
extern "C" {
    #include "../3rdparty/lua/lualib.h"
}
#include "../ui/screen_navigator.hpp"
#include "../ui/ui_manager.hpp"

// ============================================================================
// [SECURITY ARCHITECTURE NOTICE]
// This symbol export table exposes RAW kernel functions (e.g. VFS, Heap, Semaphores) 
// without any SVC privilege boundary transitions. 
// 
// CRITICAL: This table MUST ONLY be provided to TRUSTED, PRIVILEGED IN-KERNEL 
// modules (such as the built-in Lua engine running in Ring 0 / Kernel Privilege).
// It completely bypasses seL4-style capability checks and MPU sandboxing.
// DO NOT expose this symtab to unprivileged user-space sandboxes, otherwise 
// they will have unrestricted access to all kernel internals, causing either 
// MemManage Faults or complete privilege escalation.
// ============================================================================


// Forward declare sys_print wrapper or we can just use the syscall inline version inside the ELF directly.
// To provide sys_print as a callable symbol, we create a non-inline wrapper.
extern "C" void sys_print_wrapper(const char* str) {
    // Note: since sys_print is inline, calling it here works fine.
    // However, including syscall.hpp might be problematic if not in kernel space or conflicts.
    // Let's just forward it using the SVC instruction manually to ensure it's a real function.
    __asm__ volatile (
        "mov r0, %0\n\t"
        "svc %1\n\t"
        : 
        : "r"(str), "i"(SYS_PRINT)
        : "r0", "memory"
    );
}

extern "C" void navigator_push_wrapper(UI::Screen* screen) {
    UI::ScreenNavigator::instance().push(screen);
}

const KernelSymbol kernel_symtab[] = {
    {"sys_print", reinterpret_cast<uintptr_t>(&sys_print_wrapper)},
#ifndef AURORA_HOST_TEST
    {"open", reinterpret_cast<uintptr_t>(&open)},
    {"close", reinterpret_cast<uintptr_t>(&close)},
    {"read", reinterpret_cast<uintptr_t>(&read)},
    {"write", reinterpret_cast<uintptr_t>(&write)},
    {"ioctl", reinterpret_cast<uintptr_t>(&ioctl)},
    {"lseek", reinterpret_cast<uintptr_t>(&lseek)},
    {"sleep", reinterpret_cast<uintptr_t>(&sleep)},
    {"usleep", reinterpret_cast<uintptr_t>(&usleep)},
#endif
    {"sem_init", reinterpret_cast<uintptr_t>(&sem_init)},
    {"sem_wait", reinterpret_cast<uintptr_t>(&sem_wait)},
    {"sem_post", reinterpret_cast<uintptr_t>(&sem_post)},
    {"sem_destroy", reinterpret_cast<uintptr_t>(&sem_destroy)},
    
#ifdef CONFIG_LUA_VM
    // Lua Engine
    {"luaL_newstate", reinterpret_cast<uintptr_t>(&luaL_newstate)},
    {"luaL_openlibs", reinterpret_cast<uintptr_t>(&luaL_openlibs)},
    {"lua_close", reinterpret_cast<uintptr_t>(&lua_close)},
    {"luaL_loadstring", reinterpret_cast<uintptr_t>(&luaL_loadstring)},
    {"lua_pcallk", reinterpret_cast<uintptr_t>(&lua_pcallk)}, // lua_pcall is a macro for lua_pcallk
    {"luaopen_aurora_ui", reinterpret_cast<uintptr_t>(&luaopen_aurora_ui)},
#endif
    
    // UI Engine 
    {"navigator_push", reinterpret_cast<uintptr_t>(&navigator_push_wrapper)},
};

const int kernel_symtab_size = sizeof(kernel_symtab) / sizeof(kernel_symtab[0]);
