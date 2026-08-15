/**
 * @file string_util.hpp
 * @brief Shared ASCII string helpers for February (zero-heap)
 *
 * Used by wake_word, intent_engine, softbus_stub. Keep tiny for M0+/M3.
 */
#ifndef AURORA_FEBRUARY_STRING_UTIL_HPP
#define AURORA_FEBRUARY_STRING_UTIL_HPP

#include <cstddef>

namespace aurora {
namespace february {

inline char tolower_ascii(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

/** Case-insensitive substring search. Empty needle → false. */
inline bool contains_ci(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) {
        return false;
    }
    for (const char* p = hay; *p; ++p) {
        const char* h = p;
        const char* n = needle;
        while (*h && *n && tolower_ascii(*h) == tolower_ascii(*n)) {
            ++h;
            ++n;
        }
        if (!*n) {
            return true;
        }
    }
    return false;
}

/** Copy up to dest_size-1 chars; always NUL-terminate. Returns length written. */
inline std::size_t copy_cstr(char* dest, std::size_t dest_size, const char* src) {
    if (!dest || dest_size == 0) {
        return 0;
    }
    if (!src) {
        dest[0] = '\0';
        return 0;
    }
    std::size_t i = 0;
    for (; src[i] && i + 1 < dest_size; ++i) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
    return i;
}

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_STRING_UTIL_HPP
