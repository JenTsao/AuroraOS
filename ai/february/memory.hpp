/**
 * @file memory.hpp
 * @brief Fixed-slot session memory for February Phase 1
 */
#ifndef AURORA_FEBRUARY_MEMORY_HPP
#define AURORA_FEBRUARY_MEMORY_HPP

#include "types.hpp"

namespace aurora {
namespace february {

class SessionMemory {
public:
    static SessionMemory& instance() {
        static SessionMemory m;
        return m;
    }

    void note_intent(const Intent& in, uint32_t now_ms) {
        last_intent_ = in;
        last_intent_ms_ = now_ms;
        ++intent_count_;
    }

    void note_speak(const char* msg, uint32_t now_ms) {
        last_speak_ms_ = now_ms;
        unsigned i = 0;
        if (msg) {
            for (; msg[i] && i + 1 < sizeof(last_speak_); ++i)
                last_speak_[i] = msg[i];
        }
        last_speak_[i] = '\0';
        ++speak_count_;
    }

    void note_dnd(bool on) {
        dnd_known_ = true;
        dnd_on_ = on;
    }

    const Intent& last_intent() const { return last_intent_; }
    uint32_t last_intent_ms() const { return last_intent_ms_; }
    const char* last_speak() const { return last_speak_; }
    uint32_t intent_count() const { return intent_count_; }
    uint32_t speak_count() const { return speak_count_; }
    bool dnd_known() const { return dnd_known_; }
    bool dnd_on() const { return dnd_on_; }

    void clear() {
        last_intent_.clear();
        last_intent_ms_ = 0;
        last_speak_[0] = '\0';
        last_speak_ms_ = 0;
        intent_count_ = 0;
        speak_count_ = 0;
        dnd_known_ = false;
        dnd_on_ = false;
    }

private:
    SessionMemory() = default;
    Intent   last_intent_{};
    uint32_t last_intent_ms_ = 0;
    char     last_speak_[96] = {};
    uint32_t last_speak_ms_  = 0;
    uint32_t intent_count_   = 0;
    uint32_t speak_count_    = 0;
    bool     dnd_known_      = false;
    bool     dnd_on_         = false;
};

}  // namespace february
}  // namespace aurora

#endif
