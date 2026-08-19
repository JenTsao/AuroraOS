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

inline bool is_space_or_punct(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == ',' || c == '.' || c == '!' || c == '?' ||
           c == ':' || c == ';' || c == '-' || c == '_' || c == '\0';
}

/** Case-insensitive exact string comparison. */
inline bool equals_ci(const char* s1, const char* s2) {
    if (s1 == s2) return true;
    if (!s1 || !s2) return false;
    while (*s1 && *s2) {
        if (tolower_ascii(*s1) != tolower_ascii(*s2)) return false;
        ++s1;
        ++s2;
    }
    return (*s1 == '\0' && *s2 == '\0');
}

/** Case-insensitive prefix check. */
inline bool starts_with_ci(const char* hay, const char* prefix) {
    if (!hay || !prefix) return false;
    while (*prefix) {
        if (tolower_ascii(*hay) != tolower_ascii(*prefix)) return false;
        ++hay;
        ++prefix;
    }
    return true;
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

/** Case-insensitive whole-word or token-boundary matching. */
inline bool contains_word_ci(const char* hay, const char* word) {
    if (!hay || !word || !*word) {
        return false;
    }
    const char* prev = nullptr;
    for (const char* p = hay; *p; prev = p, ++p) {
        if (prev == nullptr || is_space_or_punct(*prev)) {
            const char* h = p;
            const char* w = word;
            while (*h && *w && tolower_ascii(*h) == tolower_ascii(*w)) {
                ++h;
                ++w;
            }
            if (!*w && is_space_or_punct(*h)) {
                return true;
            }
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

inline std::size_t safe_strlen(const char* s) {
    if (!s) return 0;
    std::size_t len = 0;
    while (s[len]) ++len;
    return len;
}

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_STRING_UTIL_HPP
