/**
 * @file working_memory.hpp
 * @brief WorkingMemory — 64-slot ring, ~5 min window, Q8 temporal decay
 *
 * Zero-heap. Suitable for M0+ (only tier enabled by default on tiny MCUs).
 */
#ifndef AURORA_FEBRUARY_WORKING_MEMORY_HPP
#define AURORA_FEBRUARY_WORKING_MEMORY_HPP

#include "types.hpp"
#include "config.hpp"
#include "crit.hpp"

namespace aurora {
namespace february {

#ifndef FEBRUARY_WORKING_MEMORY_SLOTS
#define FEBRUARY_WORKING_MEMORY_SLOTS 64
#endif

#ifndef FEBRUARY_WORKING_MEMORY_WINDOW_MS
#define FEBRUARY_WORKING_MEMORY_WINDOW_MS 300000u
#endif

enum class WmKind : uint8_t {
    None = 0,
    Intent,
    Sensor,
    Speak,
    Context,
    Action,
    Remote,
    Custom
};

struct WorkingSlot {
    uint32_t timestamp_ms  = 0;
    WmKind   kind          = WmKind::None;
    ConfidenceQ8 strength_q8 = 0;
    uint8_t  tag           = 0;
    uint32_t a             = 0;
    uint32_t b             = 0;
};

class WorkingMemory {
public:
    static WorkingMemory& instance() {
        static WorkingMemory wm;
        return wm;
    }

    static constexpr unsigned capacity() { return FEBRUARY_WORKING_MEMORY_SLOTS; }
    static constexpr uint32_t window_ms() { return FEBRUARY_WORKING_MEMORY_WINDOW_MS; }

    void push(WmKind kind, uint8_t tag, uint32_t a, uint32_t b,
              ConfidenceQ8 strength_q8, uint32_t now_ms) {
        FebruaryCrit::Guard g;
        decay_unlocked(now_ms);
        const unsigned next = (head_ + 1u) % FEBRUARY_WORKING_MEMORY_SLOTS;
        if (next == tail_ && count_ == FEBRUARY_WORKING_MEMORY_SLOTS) {
            tail_ = (tail_ + 1u) % FEBRUARY_WORKING_MEMORY_SLOTS;
            --count_;
            ++drop_count_;
        }
        WorkingSlot& s = slots_[head_];
        s.timestamp_ms = now_ms;
        s.kind = kind;
        s.strength_q8 = strength_q8 ? strength_q8 : 255;
        s.tag = tag;
        s.a = a;
        s.b = b;
        head_ = next;
        if (count_ < FEBRUARY_WORKING_MEMORY_SLOTS) {
            ++count_;
        }
    }

    void note_intent(const Intent& in, uint32_t now_ms) {
        push(WmKind::Intent, 0, static_cast<uint32_t>(in.type),
             in.confidence_x1000, conf_from_x1000(in.confidence_x1000), now_ms);
    }

    void note_sensor(SensorKind sk, ConfidenceQ8 conf, uint32_t now_ms) {
        push(WmKind::Sensor, static_cast<uint8_t>(sk),
             static_cast<uint32_t>(sk), conf, conf, now_ms);
    }

    void note_speak(uint32_t now_ms) {
        push(WmKind::Speak, 0, 0, 0, 200, now_ms);
    }

    void decay(uint32_t now_ms) {
        FebruaryCrit::Guard g;
        decay_unlocked(now_ms);
    }

    unsigned count() const {
        FebruaryCrit::Guard g;
        return count_;
    }

    uint32_t drop_count() const { return drop_count_; }

    template <typename Fn>
    void for_each_recent(uint32_t now_ms, Fn&& fn) const {
        FebruaryCrit::Guard g;
        if (count_ == 0) return;
        unsigned idx = (head_ + FEBRUARY_WORKING_MEMORY_SLOTS - 1u) %
                       FEBRUARY_WORKING_MEMORY_SLOTS;
        for (unsigned n = 0; n < count_; ++n) {
            const WorkingSlot& s = slots_[idx];
            if (s.kind != WmKind::None) {
                WorkingSlot view = s;
                view.strength_q8 = decayed_strength(s, now_ms);
                if (view.strength_q8 > 0) {
                    if (!fn(view)) return;
                }
            }
            idx = (idx + FEBRUARY_WORKING_MEMORY_SLOTS - 1u) %
                  FEBRUARY_WORKING_MEMORY_SLOTS;
        }
    }

    bool peek_last_intent(IntentType* out_type, ConfidenceQ8* out_str,
                          uint32_t now_ms) const {
        bool found = false;
        IntentType best_t = IntentType::None;
        ConfidenceQ8 best_s = 0;
        for_each_recent(now_ms, [&](const WorkingSlot& s) {
            if (s.kind == WmKind::Intent && s.strength_q8 > best_s) {
                best_s = s.strength_q8;
                best_t = static_cast<IntentType>(s.a);
                found = true;
            }
            return true;
        });
        if (found && out_type) *out_type = best_t;
        if (found && out_str) *out_str = best_s;
        return found;
    }

    void clear() {
        FebruaryCrit::Guard g;
        head_ = tail_ = count_ = 0;
        drop_count_ = 0;
        for (unsigned i = 0; i < FEBRUARY_WORKING_MEMORY_SLOTS; ++i) {
            slots_[i] = WorkingSlot{};
        }
    }

private:
    WorkingMemory() = default;

    static ConfidenceQ8 conf_from_x1000(uint32_t x1000) {
        if (x1000 >= 1000) return 255;
        return static_cast<ConfidenceQ8>((x1000 * 255u) / 1000u);
    }

    static ConfidenceQ8 decayed_strength(const WorkingSlot& s, uint32_t now_ms) {
        if (s.kind == WmKind::None || s.strength_q8 == 0) return 0;
        const uint32_t age = (now_ms >= s.timestamp_ms)
                                 ? (now_ms - s.timestamp_ms)
                                 : 0u;
        if (age >= FEBRUARY_WORKING_MEMORY_WINDOW_MS) return 0;
        const uint32_t remain = FEBRUARY_WORKING_MEMORY_WINDOW_MS - age;
        const uint32_t scaled =
            (static_cast<uint32_t>(s.strength_q8) * remain) /
            FEBRUARY_WORKING_MEMORY_WINDOW_MS;
        return static_cast<ConfidenceQ8>(scaled > 255 ? 255 : scaled);
    }

    void decay_unlocked(uint32_t now_ms) {
        if (count_ == 0) return;
        unsigned idx = tail_;
        for (unsigned n = 0; n < count_; ++n) {
            WorkingSlot& s = slots_[idx];
            if (decayed_strength(s, now_ms) == 0) {
                s = WorkingSlot{};
            }
            idx = (idx + 1u) % FEBRUARY_WORKING_MEMORY_SLOTS;
        }
        while (count_ > 0) {
            if (slots_[tail_].kind != WmKind::None &&
                decayed_strength(slots_[tail_], now_ms) > 0) {
                break;
            }
            slots_[tail_] = WorkingSlot{};
            tail_ = (tail_ + 1u) % FEBRUARY_WORKING_MEMORY_SLOTS;
            --count_;
        }
    }

    WorkingSlot slots_[FEBRUARY_WORKING_MEMORY_SLOTS]{};
    unsigned    head_ = 0;
    unsigned    tail_ = 0;
    unsigned    count_ = 0;
    uint32_t    drop_count_ = 0;
};

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_WORKING_MEMORY_HPP
