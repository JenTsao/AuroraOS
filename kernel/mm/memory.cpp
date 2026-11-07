#include "memory.hpp"
#include <stddef.h>
#ifndef AURORA_HOST_TEST
#include "autoconf.h"
#endif
#include "arch_api.hpp"

// std::align_val_t 属于 <new>。裸机工具链不一定提供 libstdc++ 头（RV32 走
// picolibc，无 <new>），故有则用、无则前置声明该标记类型；其底层类型必须与
// 标准库一致（size_t），否则过对齐 delete 的符号名对不上。
#if defined(__has_include)
#if __has_include(<new>)
#include <new>
#define AURORA_HAS_STD_NEW 1
#endif
#endif

#ifndef AURORA_HAS_STD_NEW
namespace std {
enum class align_val_t : decltype(sizeof(0));
}
#endif

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

// C++17 过对齐类型（alignof 超过默认 new 对齐，例如 virtio 驱动里 alignas(4096)
// 的描述符环）的销毁路径。GCC 为这类类型的虚析构函数生成 deleting destructor
// 时会无条件引用带 std::align_val_t 的 delete，即使对象从不走 new 表达式 ——
// 缺符号的表现是链接期 "undefined reference to operator delete(void*, unsigned
// long, std::align_val_t)"。内核不做过对齐堆分配（此类对象均为静态或板级实例），
// 故这里只补符号对齐、归还路径与普通 delete 一致。
void operator delete(void* ptr, std::align_val_t) noexcept {
    KernelHeap::instance().deallocate(ptr);
}

void operator delete[](void* ptr, std::align_val_t) noexcept {
    KernelHeap::instance().deallocate(ptr);
}

void operator delete(void* ptr, size_t size, std::align_val_t) noexcept {
    (void)size;
    KernelHeap::instance().deallocate(ptr);
}

void operator delete[](void* ptr, size_t size, std::align_val_t) noexcept {
    (void)size;
    KernelHeap::instance().deallocate(ptr);
}

// 解决 C++ 全局静态对象析构的链接缺失问题
void* __dso_handle = nullptr;

extern "C" {
int __aeabi_atexit(void*, void (*)(void*), void*) {
    return 0;
}
}
