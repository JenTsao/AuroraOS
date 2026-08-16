/**
 * @file event_bus.hpp
 * @brief Fixed-size event bus for February
 *
 * No dynamic allocation. Ring buffer + subscriber table.
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

    bool subscribe(EventType type, EventHandler handler, void* user = nullptr) {
        if (!handler) {
            return false;
        }
        FebruaryCrit::Guard g;
        for (unsigned i = 0; i < kMaxEventSubscribers; ++i) {
            if (slots_[i].handler == nullptr) {
                slots_[i].type    = type;
                slots_[i].handler = handler;
                slots_[i].user    = user;
                return true;
            }
        }
        return false;
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
        unsigned next = (head_ + 1) & (kEventQueueDepth - 1);
        if (next == tail_) {
            tail_ = (tail_ + 1) & (kEventQueueDepth - 1);
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
                tail_ = (tail_ + 1) & (kEventQueueDepth - 1);
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
            slots_[i].handler = nullptr;
            slots_[i].user    = nullptr;
        }
    }

    unsigned pending() const {
        FebruaryCrit::Guard g;
        if (head_ >= tail_) {
            return head_ - tail_;
        }
        return kEventQueueDepth - tail_ + head_;
    }

    uint32_t drop_count() const { return drop_count_; }

    bool has_local_intent() const {
        FebruaryCrit::Guard g;
        for (unsigned i = tail_; i != head_; i = (i + 1) & (kEventQueueDepth - 1)) {
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
        EventType    type    = EventType::None;
        EventHandler handler = nullptr;
        void*        user    = nullptr;
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
