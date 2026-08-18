/**
 * @file event_bus.hpp
 * @brief Fixed-size event bus for February
 *
 * No dynamic allocation. Ring buffer + priority subscriber table.
 *
 * Threading (Phase 2.2):
 *   - publish() / process() take FebruaryCrit::Guard when hooks are bound.
 *   - Typical: sensors publish; single February service task processes.
 *   - ISR + task: bind IRQ disable or mutex via FebruaryCrit::set().
 */
#ifndef AURORA_FEBRUARY_EVENT_BUS_HPP
#define AURORA_FEBRUARY_EVENT_BUS_HPP

#include "types.hpp"
#include "crit.hpp"

namespace aurora {
namespace february {

using EventHandler = void (*)(const Event& ev, void* user);

class EventBus {
public:
    static EventBus& instance() {
        static EventBus bus;
        return bus;
    }

    bool subscribe(EventType type, EventHandler handler, void* user = nullptr,
                   SubPriority prio = SubPriority::Normal) {
        if (!handler) {
            return false;
        }
        FebruaryCrit::Guard g;
        int free_idx = -1;
        for (unsigned i = 0; i < kMaxEventSubscribers; ++i) {
            if (slots_[i].handler == nullptr) {
                free_idx = static_cast<int>(i);
                break;
            }
        }
        if (free_idx < 0) return false;
        slots_[free_idx].type     = type;
        slots_[free_idx].handler  = handler;
        slots_[free_idx].user     = user;
        slots_[free_idx].priority = static_cast<uint8_t>(prio);
        for (unsigned i = 1; i < kMaxEventSubscribers; ++i) {
            auto key = slots_[i];
            int j = static_cast<int>(i) - 1;
            while (j >= 0 && slots_[j].priority < key.priority) {
                slots_[j + 1] = slots_[j];
                --j;
            }
            slots_[j + 1] = key;
        }
        return true;
    }

    void unsubscribe(EventHandler handler) {
        FebruaryCrit::Guard g;
        for (unsigned i = 0; i < kMaxEventSubscribers; ++i) {
            if (slots_[i].handler == handler) {
                slots_[i].handler = nullptr;
                slots_[i].user    = nullptr;
            }
        }
    }

    bool publish(const Event& ev) {
        FebruaryCrit::Guard g;
        unsigned next = (head_ + 1) % kEventQueueDepth;
        if (next == tail_) {
            tail_ = (tail_ + 1) % kEventQueueDepth;
            ++drop_count_;
        }
        queue_[head_] = ev;
        head_ = next;
        return true;
    }

    unsigned process(unsigned max_events = 8) {
        unsigned handled = 0;
        while (handled < max_events) {
            Event ev;
            {
                FebruaryCrit::Guard g;
                if (tail_ == head_) {
                    break;
                }
                ev = queue_[tail_];
                tail_ = (tail_ + 1) % kEventQueueDepth;
            }
            dispatch(ev);
            ++handled;
        }
        return handled;
    }

    void clear() {
        FebruaryCrit::Guard g;
        head_ = tail_ = 0;
        drop_count_ = 0;
        for (unsigned i = 0; i < kMaxEventSubscribers; ++i) {
            slots_[i].handler  = nullptr;
            slots_[i].user     = nullptr;
            slots_[i].priority = 0;
        }
    }

    uint32_t drop_count() const { return drop_count_; }

    unsigned pending() const {
        FebruaryCrit::Guard g;
        if (head_ >= tail_) {
            return head_ - tail_;
        }
        return kEventQueueDepth - tail_ + head_;
    }

    bool has_local_intent() const {
        FebruaryCrit::Guard g;
        for (unsigned i = tail_; i != head_; i = (i + 1) % kEventQueueDepth) {
            const EventType t = queue_[i].type;
            if (t == EventType::IntentDetected ||
                t == EventType::ProactiveTrigger) {
                return true;
            }
        }
        return false;
    }

private:
    EventBus() = default;

    void dispatch(const Event& ev) {
        for (unsigned i = 0; i < kMaxEventSubscribers; ++i) {
            EventHandler h = nullptr;
            void* user = nullptr;
            EventType typ = EventType::None;
            {
                FebruaryCrit::Guard g;
                h = slots_[i].handler;
                user = slots_[i].user;
                typ = slots_[i].type;
            }
            if (h && (typ == EventType::None || typ == ev.type)) {
                h(ev, user);
            }
        }
    }

    struct Slot {
        EventType    type     = EventType::None;
        EventHandler handler  = nullptr;
        void*        user     = nullptr;
        uint8_t      priority = 0;
    };

    Slot     slots_[kMaxEventSubscribers]{};
    Event    queue_[kEventQueueDepth]{};
    unsigned head_ = 0;
    unsigned tail_ = 0;
    uint32_t drop_count_ = 0;
};

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_EVENT_BUS_HPP
