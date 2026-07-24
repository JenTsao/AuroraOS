#include "memory.hpp"
#include <stddef.h>
#ifndef AURORA_HOST_TEST
#include "autoconf.h"
#endif
#include "arch_api.hpp"

// 声明链接脚本里暴露的外部边界符号
extern "C" {
    extern char* _heap_start;
    extern char* _heap_end;
}

void* operator new(size_t size) {
#ifdef CONFIG_NO_DYNAMIC_ALLOCATION
    Arch::disable_interrupts();
    while (true) {} // PANIC: Dynamic allocation is disabled
#else
    void* p = KernelHeap::instance().allocate(size);
    if (!p) {
        Arch::disable_interrupts();
        while (true) {} // PANIC: Heap exhausted
    }
    return p;
#endif
}

void* operator new[](size_t size) {
#ifdef CONFIG_NO_DYNAMIC_ALLOCATION
    Arch::disable_interrupts();
    while (true) {} // PANIC: Dynamic allocation is disabled
#else
    void* p = KernelHeap::instance().allocate(size);
    if (!p) {
        Arch::disable_interrupts();
        while (true) {} // PANIC: Heap exhausted
    }
    return p;
#endif
}

// 覆写标准 C++ 全局 delete 运算符
void operator delete(void* ptr) noexcept {
    KernelHeap::instance().deallocate(ptr);
}

void operator delete[](void* ptr) noexcept {
    KernelHeap::instance().deallocate(ptr);
}

// 针对 C++ 虚函数或特殊销毁机制的防报错桩函数
void operator delete(void* ptr, size_t size) noexcept {
    (void)size;
    KernelHeap::instance().deallocate(ptr);
}

void operator delete[](void* ptr, size_t size) noexcept {
    (void)size;
    KernelHeap::instance().deallocate(ptr);
}

// 解决 C++ 全局静态对象析构的链接缺失问题
extern "C" {
    void* __dso_handle = nullptr;
    int __aeabi_atexit(void*, void (*)(void*), void*) {
        return 0;
    }
}
