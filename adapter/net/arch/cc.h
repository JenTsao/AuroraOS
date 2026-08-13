#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>

// Types required by lwIP
typedef uint8_t u8_t;
typedef int8_t s8_t;
typedef uint16_t u16_t;
typedef int16_t s16_t;
typedef uint32_t u32_t;
typedef int32_t s32_t;
typedef uintptr_t mem_ptr_t;

// Compiler hints for struct packing
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

// Standard diagnostic output
#ifdef __cplusplus
extern "C" {
#endif
void sys_print_c(const char* msg);
#ifdef __cplusplus
}
#endif

// Note: lwIP calls LWIP_PLATFORM_DIAG with printf-like arguments (e.g. ("foo %d", 1)).
// We don't have full printf, so we'll stub it out for now to avoid compilation errors.
#define LWIP_PLATFORM_DIAG(x)

#define LWIP_PLATFORM_ASSERT(x)                                                                                        \
    do {                                                                                                               \
        sys_print_c("Assertion \"");                                                                                   \
        sys_print_c(x);                                                                                                \
        sys_print_c("\" failed\r\n");                                                                                  \
        while (1)                                                                                                      \
            ;                                                                                                          \
    } while (0)

#endif // LWIP_ARCH_CC_H
