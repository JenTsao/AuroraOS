#ifndef AURORA_PLACEMENT_NEW_HPP
#define AURORA_PLACEMENT_NEW_HPP

#include <stddef.h>

#ifdef AURORA_HOST_TEST
// Host tests always have access to the standard library
#include <new>
#else
// Bare-metal freestanding environments may lack <new> (e.g. RISC-V none-elf without libstdc++)
// We provide a minimal inline placement new definition if the standard one isn't pulled in.
#if !defined(__PLACEMENT_NEW_INLINE) && !defined(__PLACEMENT_VEC_NEW_INLINE)
#define __PLACEMENT_NEW_INLINE
#define __PLACEMENT_VEC_NEW_INLINE

inline void* operator new(size_t, void* p) throw() { return p; }
inline void* operator new[](size_t, void* p) throw() { return p; }
inline void operator delete(void*, void*) throw() {}
inline void operator delete[](void*, void*) throw() {}

#endif // __PLACEMENT_NEW_INLINE
#endif // AURORA_HOST_TEST

#endif // AURORA_PLACEMENT_NEW_HPP
