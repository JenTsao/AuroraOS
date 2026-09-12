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

#include "string_util.hpp"
#include <cstddef>

namespace aurora {
namespace february {

/**
 * Holds the product wake word (e.g. "hey february", "二月", …).
 * Default is empty — leave unset until the product decides.
 */
class WakeWordConfig {
public:
    static WakeWordConfig& instance();

    /** Set wake word. Pass nullptr or "" to clear (no gate). Max 31 chars. */
    void set(const char* word) {
        if (!word) {
            word_[0] = '\0';
            return;
        }
        copy_cstr(word_, sizeof(word_), word);
    }

    const char* get() const { return word_; }

    bool configured() const { return word_[0] != '\0'; }

    /**
     * If no wake word is configured, returns true (accept all).
     * If configured, returns true only when `utterance` contains the wake word
     * (ASCII case-insensitive substring).
     */
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
    constexpr WakeWordConfig() : word_{} {}

    static WakeWordConfig storage_;

    char word_[32];
};

// Out-of-line definition of the guard-free singleton storage. Constant-
// initialized (constexpr ctor), so no __cxa_guard and no dynamic initializer:
// instance() is safe to call from an ISR.
inline WakeWordConfig WakeWordConfig::storage_{};

inline WakeWordConfig& WakeWordConfig::instance() {
    return storage_;
}

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_WAKE_WORD_HPP
