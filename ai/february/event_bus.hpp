/**
 * @file event_bus.hpp
 * @brief Fixed-size event bus for February
 *
 * No dynamic allocation. Ring buffer + subscriber table.
 *
 * Threading model (Phase 1):
 *   - publish() and process() must not run concurrently.
 *   - Typical use: sensors/tasks publish; single February service task calls process().
 *   - Not safe for ISR + task without an external IrqGuard / critical section.
 */
#ifndef AURORA_FEBRUARY_EVENT_BUS_HPP
#define AURORA_FEBRUARY_EVENT_BUS_HPP

#include "types.hpp"

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
        if (!handler) return false;
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
        for (unsigned i = 0; i < kMaxEventSubscribers; ++i) {
            if (slots_[i].handler == handler) {
                slots_[i].handler = nullptr;
                slots_[i].user    = nullptr;
            }
        }
    }

    bool publish(const Event& ev) {
        unsigned next = (head_ + 1) % kEventQueueDepth;
        if (next == tail_) {
            tail_ = (tail_ + 1) % kEventQueueDepth;
        }
        queue_[head_] = ev;
        head_ = next;
        return true;
    }

    unsigned process(unsigned max_events = 8) {
        unsigned handled = 0;
        while (tail_ != head_ && handled < max_events) {
            const Event& ev = queue_[tail_];
            dispatch(ev);
            tail_ = (tail_ + 1) % kEventQueueDepth;
            ++handled;
        }
        return handled;
    }

    void clear() {
        head_ = tail_ = 0;
        for (unsigned i = 0; i < kMaxEventSubscribers; ++i) {
            slots_[i].handler = nullptr;
            slots_[i].user    = nullptr;
        }
    }

private:
    EventBus() = default;

    void dispatch(const Event& ev) {
        for (unsigned i = 0; i < kMaxEventSubscribers; ++i) {
            if (slots_[i].handler &&
                (slots_[i].type == EventType::None || slots_[i].type == ev.type)) {
                slots_[i].handler(ev, slots_[i].user);
            }
        }
    }

    struct Slot {
        EventType    type    = EventType::None;
        EventHandler handler = nullptr;
        void*        user    = nullptr;
    };

    Slot   slots_[kMaxEventSubscribers]{};
    Event  queue_[kEventQueueDepth]{};
    unsigned head_ = 0;
    unsigned tail_ = 0;
};

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_EVENT_BUS_HPP
