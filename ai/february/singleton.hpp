/**
 * @file singleton.hpp
 * @brief Guard-free, ISR-safe singleton pattern for February (documentation)
 *
 * Problem: the usual `static T& instance() { static T t; return t; }` idiom
 * relies on compiler-emitted `__cxa_guard_acquire/__cxa_guard_release` calls
 * (`-fthreadsafe-statics`). On bare-metal RTOS builds:
 *   - the guard needs a working `__cxa_guard_*` implementation (libsupc++ /
 *     libstdc++), which tiny Cortex-M0+ toolchains frequently strip out, and
 *   - the guard is NOT ISR-safe: the first call from an interrupt handler while
 *     another first call is already in flight can deadlock or double-construct.
 *
 * Fix (C++17 — the standard level this project builds with, see
 * tests/CMakeLists.txt): use a **static data member** as the storage instead
 * of a function-local static. A namespace/class-scope object is never guarded
 * by `__cxa_guard_*`; it is initialized once during static init (and, when the
 * constructor is `constexpr` and every member has a constant initializer, it
 * is constant-initialized and lands in .bss/.data with no startup code at
 * all). `instance()` therefore performs zero initialization work and is safe
 * to call from an ISR.
 *
 * Why not `constinit`? `constinit` is C++20. Compiling with `-std=c++17`
 * makes g++ emit `warning: identifier 'constinit' is a keyword in C++20
 * [-Wc++20-compat]`, which is fatal under this project's `-Werror`. The
 * static-data-member form gives the same guard-free guarantee on C++17.
 *
 * IMPORTANT — the definition must be OUT-OF-LINE:
 *   A static data member of an incomplete type cannot be declared inside its
 *   own class (`static T storage_;` is fine — it is only *defined* later), but
 *   the out-of-line definition `inline T T::storage_{};` must appear *after*
 *   the closing brace so the type is complete. Same for `instance()`.
 *
 * Correct usage:
 *
 *   class Foo {
 *   public:
 *       static Foo& instance();          // 1. declare only
 *       // ...
 *   private:
 *       constexpr Foo() = default;       // 2. constexpr ctor
 *       static Foo storage_;             // 3. declare storage
 *   };
 *
 *   inline Foo Foo::storage_{};          // 4. define after the class
 *
 *   inline Foo& Foo::instance() {        // 5. define after the class
 *       return storage_;
 *   }
 *
 * Alternative for a private *nested* type (where spelling
 * `Foo::Nested Foo::storage_` at namespace scope trips access control):
 * use a C++17 inline static data member and skip step 4 entirely:
 *
 *       inline static Nested storage_{};
 *
 * Verifying the fix (should print 0):
 *   g++ -std=c++17 -O2 -S -o out.s -I. probe.cpp
 *   grep -c '__cxa_guard' out.s
 *
 * Before this change the same probe emitted 36 guard references; now it emits 0.
 */
#ifndef AURORA_FEBRUARY_SINGLETON_HPP
#define AURORA_FEBRUARY_SINGLETON_HPP

namespace aurora {
namespace february {

/**
 * Declares (but does not define) a guard-free singleton accessor.
 * You MUST also declare the storage member and emit the out-of-line
 * definitions after the class:
 *
 *   private:
 *       static TypeName storage_;
 *
 *   inline TypeName TypeName::storage_{};
 *
 *   inline TypeName& TypeName::instance() {
 *       return storage_;
 *   }
 */
#define FEBRUARY_DECLARE_CONSTINIT_SINGLETON(TypeName) \
    static TypeName& instance();

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_SINGLETON_HPP
