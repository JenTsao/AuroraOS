/**
 * @file memory.hpp
 * @brief SessionMemory facade — Working + optional Episodic + WorldModel tiers
 *
 * Tier budget (Kconfig):
 *   M0+  : WorkingMemory only
 *   M3/M4: + EpisodicMemory + WorldModel (DeviceGraph)
 *
 * Dialogue history ring buffer + multi-turn anaphora resolution support.
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

constexpr unsigned kSessionHistoryCapacity = 4;

struct HistoryTurn {
    Intent   intent{};
    uint32_t intent_ms = 0;
    char     speak[96] = {};
    uint32_t speak_ms  = 0;
    bool     valid     = false;

    void clear() {
        intent.clear();
        intent_ms = 0;
        speak[0] = '\0';
        speak_ms = 0;
        valid = false;
    }
};

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

        // Push new turn into history ring buffer
        history_[head_].clear();
        history_[head_].intent = in;
        history_[head_].intent_ms = now_ms;
        history_[head_].valid = true;

        head_ = (head_ + 1) % kSessionHistoryCapacity;
        if (history_count_ < kSessionHistoryCapacity) {
            ++history_count_;
        }
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

        // Attach speak message to the current latest turn in the ring buffer
        if (history_count_ > 0) {
            unsigned latest_idx = (head_ + kSessionHistoryCapacity - 1) % kSessionHistoryCapacity;
            if (history_[latest_idx].valid) {
                history_[latest_idx].speak_ms = now_ms;
                unsigned j = 0;
                if (msg) {
                    for (; msg[j] && j + 1 < sizeof(history_[latest_idx].speak); ++j) {
                        history_[latest_idx].speak[j] = msg[j];
                    }
                }
                history_[latest_idx].speak[j] = '\0';
            }
        }
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

    // Ring buffer accessors
    unsigned history_count() const { return history_count_; }
    static constexpr unsigned history_capacity() { return kSessionHistoryCapacity; }

    /// depth = 0: latest turn, depth = 1: 1 turn ago, etc.
    const HistoryTurn* get_history_turn(unsigned depth = 0) const {
        if (depth >= history_count_) return nullptr;
        unsigned idx = (head_ + kSessionHistoryCapacity - 1 - depth) % kSessionHistoryCapacity;
        return &history_[idx];
    }

    static bool is_actionable_state_intent(IntentType t) {
        switch (t) {
        case IntentType::SetDoNotDisturb:
        case IntentType::StartFitness:
        case IntentType::StopFitness:
        case IntentType::OpenApp:
        case IntentType::CloseApp:
        case IntentType::PromoteApp:
        case IntentType::DemoteApp:
        case IntentType::SetPowerMode:
            return true;
        default:
            return false;
        }
    }

    /// Search backwards through history ring buffer for the most recent actionable state intent
    const Intent* find_last_actionable_intent() const {
        for (unsigned depth = 0; depth < history_count_; ++depth) {
            unsigned idx = (head_ + kSessionHistoryCapacity - 1 - depth) % kSessionHistoryCapacity;
            if (!history_[idx].valid) continue;
            const Intent& in = history_[idx].intent;
            if (is_actionable_state_intent(in.type)) {
                return &in;
            }
        }
        return nullptr;
    }

    /// Search backwards through history ring buffer for the most recent intent matching specific type
    const Intent* find_last_intent(IntentType type = IntentType::None) const {
        for (unsigned depth = 0; depth < history_count_; ++depth) {
            unsigned idx = (head_ + kSessionHistoryCapacity - 1 - depth) % kSessionHistoryCapacity;
            if (!history_[idx].valid) continue;
            const Intent& in = history_[idx].intent;
            if (type == IntentType::None || in.type == type) {
                return &in;
            }
        }
        return nullptr;
    }

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

        for (unsigned i = 0; i < kSessionHistoryCapacity; ++i) {
            history_[i].clear();
        }
        head_ = 0;
        history_count_ = 0;
    }

private:
    SessionMemory() = default;
    Intent       last_intent_{};
    uint32_t     last_intent_ms_ = 0;
    char         last_speak_[96] = {};
    uint32_t     last_speak_ms_  = 0;
    uint32_t     intent_count_   = 0;
    uint32_t     speak_count_    = 0;
    bool         dnd_known_      = false;
    bool         dnd_on_         = false;

    HistoryTurn  history_[kSessionHistoryCapacity]{};
    unsigned     head_          = 0;
    unsigned     history_count_ = 0;
};

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_MEMORY_HPP
