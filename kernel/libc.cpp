#include <stddef.h>
#include <stdint.h>

#include "memory.hpp"
#ifndef AURORA_HOST_TEST
#include "autoconf.h"
#endif
#include "arch_api.hpp"
#include "task.hpp"

extern "C" {

void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = static_cast<uint8_t*>(dest);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

void* memset(void* s, int c, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(s);
    for (size_t i = 0; i < n; i++) {
        p[i] = static_cast<uint8_t>(c);
    }
    return s;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const uint8_t* p1 = static_cast<const uint8_t*>(s1);
    const uint8_t* p2 = static_cast<const uint8_t*>(s2);
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] < p2[i] ? -1 : 1;
        }
    }
    return 0;
}

size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) return (unsigned char)s1[i] - (unsigned char)s2[i];
        if (s1[i] == '\0') return 0;
    }
    return 0;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

void* memmove(void* dest, const void* src, size_t n) {
    uint8_t* d = static_cast<uint8_t*>(dest);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dest;
}

int atoi(const char* str) {
    int res = 0;
    int sign = 1;
    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r' || *str == '\v' || *str == '\f') {
        str++;
    }
    if (*str == '-') { sign = -1; str++; }
    else if (*str == '+') { str++; }
    while (*str >= '0' && *str <= '9') {
        res = res * 10 + (*str - '0');
        str++;
    }
    return res * sign;
}

int* __errno_location() {
    TaskControlBlock* current = Scheduler::instance().get_current_tcb();
    if (current) {
        return &current->errno_val;
    }
    static int global_errno = 0;
    return &global_errno;
}

// _ctype_ 由 newlib 提供，不再自定义（避免多重定义链接错误）

// Wrapper for sys_print to be callable from C code (like lwIP)
#include "syscall.hpp"
void sys_print_c(const char* str) {
    sys_print(str);
}


void* malloc(size_t size) {
#ifdef CONFIG_NO_DYNAMIC_ALLOCATION
    Arch::disable_interrupts();
    while (true) {} // PANIC: Dynamic allocation is disabled
#else
    return KernelHeap::instance().allocate(size);
#endif
}

void free(void* ptr) {
    KernelHeap::instance().deallocate(ptr);
}

size_t strcspn(const char *s, const char *reject) {
    size_t count = 0;
    while (*s) {
        const char *r = reject;
        while (*r) {
            if (*s == *r) return count;
            r++;
        }
        s++;
        count++;
    }
    return count;
}

size_t strspn(const char *s, const char *accept) {
    size_t count = 0;
    while (*s) {
        const char *a = accept;
        bool found = false;
        while (*a) {
            if (*s == *a) {
                found = true;
                break;
            }
            a++;
        }
        if (!found) return count;
        s++;
        count++;
    }
    return count;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++) != '\0') {}
    return dest;
}

char *strchr(const char *s, int c) {
    while (*s != (char)c) {
        if (!*s++) return nullptr;
    }
    return const_cast<char*>(s);
}

int __popcountsi2(int a) {
    int count = 0;
    unsigned int x = static_cast<unsigned int>(a);
    while (x) {
        count += x & 1;
        x >>= 1;
    }
    return count;
}

void* realloc(void* ptr, size_t size) {
    if (size == 0) {
        free(ptr);
        return nullptr;
    }
    if (!ptr) {
        return malloc(size);
    }
    
    // 先获取 old_size，避免内部锁重入
    size_t old_size = KernelHeap::instance().get_requested_size(ptr);
    
    void* new_ptr = malloc(size);
    if (new_ptr) {
        size_t copy_size = (old_size < size) ? old_size : size;
        if (copy_size > 0) {
            memcpy(new_ptr, ptr, copy_size);
        }
        free(ptr);
    }
    return new_ptr;
}

void abort(void) {
    while(1);
}

void exit(int status) {
    (void)status;
    while(1);
}

int abs(int x) {
    return x < 0 ? -x : x;
}

float strtof(const char* nptr, char** endptr) {
    // 简易 strtof 实现
    float res = 0.0f;
    int sign = 1;
    if (*nptr == '-') { sign = -1; nptr++; }
    else if (*nptr == '+') { nptr++; }
    
    while (*nptr >= '0' && *nptr <= '9') {
        res = res * 10.0f + (*nptr - '0');
        nptr++;
    }
    if (*nptr == '.') {
        nptr++;
        float frac = 0.1f;
        while (*nptr >= '0' && *nptr <= '9') {
            res += (*nptr - '0') * frac;
            frac *= 0.1f;
            nptr++;
        }
    }
    if (endptr) *endptr = const_cast<char*>(nptr);
    return res * sign;
}

// 极简 math.h 占位，供 Lua lvm 引擎链接通过
float floorf(float x) {
    if (x >= 2147483647.0f || x <= -2147483648.0f || x != x) return x; // 避免 UB
    int i = static_cast<int>(x);
    return static_cast<float>(x < 0.0f && x != static_cast<float>(i) ? i - 1 : i);
}

float powf(float base, float exp) {
    // 极简 powf：仅支持整数指数
    if (exp == 0.0f) return 1.0f;
    int e = static_cast<int>(exp);
    float res = 1.0f;
    if (e > 0) {
        for (int i = 0; i < e; i++) res *= base;
    } else {
        for (int i = 0; i < -e; i++) res /= base;
    }
    return res;
}

float fmodf(float x, float y) {
    if (y == 0.0f) return 0.0f;
    float div = x / y;
    if (div >= 2147483647.0f || div <= -2147483648.0f || div != div) return 0.0f; // 避免 UB
    int quotient = static_cast<int>(div);
    return x - quotient * y;
}

} // extern "C"
