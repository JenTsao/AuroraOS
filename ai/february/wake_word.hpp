/**
 * @file wake_word.hpp
 * @brief Configurable wake word for February (二月)
 *
 * The wake word is NOT fixed yet. Platform / product owner sets it at runtime
 * (or via Kconfig later). Empty string means "no wake-word gate" — any utterance
 * is parsed as a command.
 */
#ifndef AURORA_FEBRUARY_WAKE_WORD_HPP
#define AURORA_FEBRUARY_WAKE_WORD_HPP

#include <cstddef>

namespace aurora {
namespace february {

class WakeWordConfig {
public:
    static WakeWordConfig& instance() {
        static WakeWordConfig w;
        return w;
    }

    void set(const char* word) {
        if (!word) {
            word_[0] = '\0';
            return;
        }
        std::size_t i = 0;
        for (; word[i] && i + 1 < sizeof(word_); ++i) {
            word_[i] = word[i];
        }
        word_[i] = '\0';
    }

    const char* get() const { return word_; }

    bool configured() const { return word_[0] != '\0'; }

    bool matches(const char* utterance) const {
        if (!configured()) {
            return true;
        }
        if (!utterance) {
            return false;
        }
        return contains_ci(utterance, word_);
    }

private:
    WakeWordConfig() { word_[0] = '\0'; }

    static char tolower_ascii(char c) {
        if (c >= 'A' && c <= 'Z') {
            return static_cast<char>(c - 'A' + 'a');
        }
        return c;
    }

    static bool contains_ci(const char* hay, const char* needle) {
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

    char word_[32];
};

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_WAKE_WORD_HPP
