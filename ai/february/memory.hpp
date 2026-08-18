/**
 * @file memory.hpp
 * @brief SessionMemory facade — Working + optional Episodic + WorldModel tiers
 *
 * Tier budget (Kconfig):
 *   M0+  : WorkingMemory only
 *   M3/M4: + EpisodicMemory + WorldModel (DeviceGraph)
 *
 * Backward-compatible API: note_intent / last_intent / note_speak / note_dnd.
 */
#ifndef AURORA_FEBRUARY_MEMORY_HPP
#define AURORA_FEBRUARY_MEMORY_HPP

#include "types.hpp"
#include "config.hpp"
#include "working_memory.hpp"

#if FEBRUARY_ENABLE_EPISODIC_MEMORY
#include "episodic_memory.hpp"
#endif
#if FEBRUARY_ENABLE_WORLD_MODEL
#include "world_model.hpp"
#endif

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
#if FEBRUARY_ENABLE_WORKING_MEMORY
        WorkingMemory::instance().note_intent(in, now_ms);
#endif
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
#if FEBRUARY_ENABLE_WORKING_MEMORY
        WorkingMemory::instance().note_speak(now_ms);
#endif
    }

    void note_dnd(bool on) {
        dnd_known_ = true;
        dnd_on_ = on;
    }

    void note_sensor(SensorKind sk, ConfidenceQ8 conf, uint32_t now_ms) {
#if FEBRUARY_ENABLE_WORKING_MEMORY
        WorkingMemory::instance().note_sensor(sk, conf, now_ms);
#else
        (void)sk; (void)conf; (void)now_ms;
#endif
    }

    void tick(uint32_t now_ms) {
#if FEBRUARY_ENABLE_WORKING_MEMORY
        WorkingMemory::instance().decay(now_ms);
#else
        (void)now_ms;
#endif
    }

    const Intent& last_intent() const { return last_intent_; }
    uint32_t last_intent_ms() const { return last_intent_ms_; }
    const char* last_speak() const { return last_speak_; }
    uint32_t intent_count() const { return intent_count_; }
    uint32_t speak_count() const { return speak_count_; }
    bool dnd_known() const { return dnd_known_; }
    bool dnd_on() const { return dnd_on_; }

#if FEBRUARY_ENABLE_WORKING_MEMORY
    WorkingMemory& working() { return WorkingMemory::instance(); }
    const WorkingMemory& working() const { return WorkingMemory::instance(); }
#endif
#if FEBRUARY_ENABLE_EPISODIC_MEMORY
    EpisodicMemory& episodic() { return EpisodicMemory::instance(); }
    const EpisodicMemory& episodic() const { return EpisodicMemory::instance(); }
#endif
#if FEBRUARY_ENABLE_WORLD_MODEL
    DeviceGraph& world() { return DeviceGraph::instance(); }
    const DeviceGraph& world() const { return DeviceGraph::instance(); }
#endif

    void clear() {
        last_intent_.clear();
        last_intent_ms_ = 0;
        last_speak_[0] = '\0';
        last_speak_ms_ = 0;
        intent_count_ = 0;
        speak_count_ = 0;
        dnd_known_ = false;
        dnd_on_ = false;
#if FEBRUARY_ENABLE_WORKING_MEMORY
        WorkingMemory::instance().clear();
#endif
#if FEBRUARY_ENABLE_EPISODIC_MEMORY
        EpisodicMemory::instance().clear();
#endif
#if FEBRUARY_ENABLE_WORLD_MODEL
        DeviceGraph::instance().clear();
#endif
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

#endif  // AURORA_FEBRUARY_MEMORY_HPP
