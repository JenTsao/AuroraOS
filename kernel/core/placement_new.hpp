#ifndef AURORA_PLACEMENT_NEW_HPP
#define AURORA_PLACEMENT_NEW_HPP
#include <stddef.h>
#if defined(__has_include)
#  if __has_include(<new>)
#    include <new>
#    define AURORA_HAS_STD_NEW 1
#  endif
#endif
#ifndef AURORA_HAS_STD_NEW
#if !defined(__PLACEMENT_NEW_INLINE) && !defined(__PLACEMENT_VEC_NEW_INLINE)
#define __PLACEMENT_NEW_INLINE
#define __PLACEMENT_VEC_NEW_INLINE
inline void* operator new(size_t, void* p) throw() { return p; }
inline void* operator new[](size_t, void* p) throw() { return p; }
inline void operator delete(void*, void*) throw() {}
inline void operator delete[](void*, void*) throw() {}
#endif
#endif
#endif
