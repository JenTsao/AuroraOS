/**
 * @file softbus_stub.hpp
 * @brief Local SoftBus RX inbox for February
 *
 * Fixed ring of remote / peer Intents. Real SoftBus RX and local loopback
 * both land here; FebruaryService::run_once drains into the core.
 *
 * Phase 2.2: publish/pop use FebruaryCrit when hooks are bound.
 */
#ifndef AURORA_FEBRUARY_SOFTBUS_STUB_HPP
#define AURORA_FEBRUARY_SOFTBUS_STUB_HPP

#include "config.hpp"
#include "types.hpp"
#include "string_util.hpp"
#include "crit.hpp"

namespace aurora {
namespace february {

#if FEBRUARY_ENABLE_SOFTBUS

constexpr unsigned kSoftBusQueueDepth = FEBRUARY_SOFTBUS_QUEUE_DEPTH;

struct SoftBusMessage {
    uint32_t peer_id      = 0;
    uint32_t timestamp_ms = 0;
    Intent   intent{};
};

class SoftBusStub {
public:
    static SoftBusStub& instance() {
        static SoftBusStub bus;
        return bus;
    }

    void clear() {
        FebruaryCrit::Guard g;
        head_ = tail_ = 0;
        drop_count_ = 0;
    }

    bool publish(uint32_t peer_id, const Intent& in, uint32_t now_ms) {
        SoftBusMessage msg;
        msg.peer_id = peer_id;
        msg.timestamp_ms = now_ms;
        msg.intent = in;
        if (peer_id) {
            msg.intent.source_id = peer_id;
        }

        FebruaryCrit::Guard g;
        unsigned next = (head_ + 1) % kSoftBusQueueDepth;
        if (next == tail_) {
            tail_ = (tail_ + 1) % kSoftBusQueueDepth;
            ++drop_count_;
        }
        queue_[head_] = msg;
        head_ = next;
        return true;
    }

    bool pop(SoftBusMessage& out) {
        FebruaryCrit::Guard g;
        if (tail_ == head_) {
            return false;
        }
        out = queue_[tail_];
        tail_ = (tail_ + 1) % kSoftBusQueueDepth;
        return true;
    }

    unsigned pending() const {
        FebruaryCrit::Guard g;
        if (head_ >= tail_) {
            return head_ - tail_;
        }
        return kSoftBusQueueDepth - tail_ + head_;
    }

    uint32_t drop_count() const { return drop_count_; }

    template <typename Fn>
    unsigned drain(unsigned max_n, Fn&& fn) {
        unsigned n = 0;
        SoftBusMessage msg;
        while (n < max_n && pop(msg)) {
            fn(msg);
            ++n;
        }
        return n;
    }

private:
    SoftBusStub() = default;

    SoftBusMessage queue_[kSoftBusQueueDepth]{};
    unsigned head_ = 0;
    unsigned tail_ = 0;
    uint32_t drop_count_ = 0;
};

#else  // !FEBRUARY_ENABLE_SOFTBUS

struct SoftBusMessage {
    uint32_t peer_id = 0;
    uint32_t timestamp_ms = 0;
    Intent   intent{};
};

class SoftBusStub {
public:
    static SoftBusStub& instance() {
        static SoftBusStub bus;
        return bus;
    }
    void clear() {}
    bool publish(uint32_t, const Intent&, uint32_t) { return false; }
    bool pop(SoftBusMessage&) { return false; }
    unsigned pending() const { return 0; }
    uint32_t drop_count() const { return 0; }
    template <typename Fn>
    unsigned drain(unsigned, Fn&&) { return 0; }
};

#endif  // FEBRUARY_ENABLE_SOFTBUS

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_SOFTBUS_STUB_HPP
