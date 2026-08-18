/**
 * @file world_model.hpp
 * @brief WorldModel / DeviceGraph — peer capabilities, room, battery, trust
 */
#ifndef AURORA_FEBRUARY_WORLD_MODEL_HPP
#define AURORA_FEBRUARY_WORLD_MODEL_HPP

#include "config.hpp"
#include "crit.hpp"
#include "string_util.hpp"
#include <cstdint>

namespace aurora {
namespace february {

#if FEBRUARY_ENABLE_WORLD_MODEL

#ifndef FEBRUARY_WORLD_MODEL_SIZE
#define FEBRUARY_WORLD_MODEL_SIZE FEBRUARY_PEER_TABLE_SIZE
#endif

enum class DeviceCap : uint16_t {
    None     = 0,
    Light    = 1u << 0,
    Speaker  = 1u << 1,
    Display  = 1u << 2,
    Sensory  = 1u << 3,
    Actuator = 1u << 4,
    Gateway  = 1u << 5
};

inline DeviceCap operator|(DeviceCap a, DeviceCap b) {
    return static_cast<DeviceCap>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
inline bool cap_has(uint16_t mask, DeviceCap c) {
    return (mask & static_cast<uint16_t>(c)) != 0;
}

struct DeviceNode {
    uint32_t peer_id      = 0;
    uint16_t caps         = 0;
    uint8_t  room_id      = 0;
    uint8_t  battery_pct  = 100;
    uint8_t  trust_q8     = 128;
    uint32_t last_seen_ms = 0;
    uint32_t last_tx_ms   = 0;
    uint32_t last_rx_ms   = 0;
    uint16_t tx_ok        = 0;
    uint16_t tx_fail      = 0;
    uint16_t rx_ok        = 0;
    bool     session_open = false;
    char     network_id[48] = {};
};

class DeviceGraph {
public:
    static DeviceGraph& instance() {
        static DeviceGraph g;
        return g;
    }

    static constexpr unsigned capacity() { return FEBRUARY_WORLD_MODEL_SIZE; }

    void clear() {
        FebruaryCrit::Guard g;
        for (unsigned i = 0; i < FEBRUARY_WORLD_MODEL_SIZE; ++i) {
            nodes_[i] = DeviceNode{};
        }
    }

    DeviceNode* touch(uint32_t peer_id, const char* network_id, uint32_t now_ms) {
        if (!peer_id) return nullptr;
        FebruaryCrit::Guard g;
        DeviceNode* n = find_unlocked(peer_id);
        if (!n) n = alloc_unlocked();
        if (!n) return nullptr;
        n->peer_id = peer_id;
        n->last_seen_ms = now_ms;
        if (network_id && network_id[0]) {
            copy_cstr(n->network_id, sizeof(n->network_id), network_id);
        }
        return n;
    }

    void set_caps(uint32_t peer_id, uint16_t caps, uint32_t now_ms) {
        DeviceNode* n = touch(peer_id, nullptr, now_ms);
        if (n) n->caps = caps;
    }

    void set_room(uint32_t peer_id, uint8_t room_id, uint32_t now_ms) {
        DeviceNode* n = touch(peer_id, nullptr, now_ms);
        if (n) n->room_id = room_id;
    }

    void set_battery(uint32_t peer_id, uint8_t pct, uint32_t now_ms) {
        DeviceNode* n = touch(peer_id, nullptr, now_ms);
        if (n) {
            n->battery_pct = pct > 100 ? 100 : pct;
            n->last_seen_ms = now_ms;
        }
    }

    void set_trust(uint32_t peer_id, uint8_t trust_q8, uint32_t now_ms) {
        DeviceNode* n = touch(peer_id, nullptr, now_ms);
        if (n) n->trust_q8 = trust_q8;
    }

    void note_tx(uint32_t peer_id, uint32_t now_ms, bool ok) {
        DeviceNode* n = touch(peer_id, nullptr, now_ms);
        if (!n) return;
        n->last_tx_ms = now_ms;
        if (ok) {
            if (n->tx_ok < 0xFFFFu) ++n->tx_ok;
        } else if (n->tx_fail < 0xFFFFu) {
            ++n->tx_fail;
        }
    }

    void note_rx(uint32_t peer_id, uint32_t now_ms) {
        DeviceNode* n = touch(peer_id, nullptr, now_ms);
        if (!n) return;
        n->last_rx_ms = now_ms;
        if (n->rx_ok < 0xFFFFu) ++n->rx_ok;
    }

    void set_session_open(uint32_t peer_id, bool open) {
        FebruaryCrit::Guard g;
        DeviceNode* n = find_unlocked(peer_id);
        if (n) n->session_open = open;
    }

    DeviceNode* find(uint32_t peer_id) {
        FebruaryCrit::Guard g;
        return find_unlocked(peer_id);
    }

    const DeviceNode* find(uint32_t peer_id) const {
        FebruaryCrit::Guard g;
        return find_unlocked(peer_id);
    }

    uint32_t route(DeviceCap need, uint8_t preferred_room,
                   uint8_t min_battery = 10) const {
        FebruaryCrit::Guard g;
        uint32_t best_id = 0;
        int32_t best_score = -1;
        for (unsigned i = 0; i < FEBRUARY_WORLD_MODEL_SIZE; ++i) {
            const DeviceNode& n = nodes_[i];
            if (!n.peer_id || !cap_has(n.caps, need)) continue;
            if (n.battery_pct < min_battery) continue;
            int32_t score = static_cast<int32_t>(n.trust_q8);
            score += static_cast<int32_t>(n.battery_pct);
            if (preferred_room != 0 && n.room_id == preferred_room) {
                score += 1000;
            }
            if (n.session_open) score += 50;
            if (score > best_score) {
                best_score = score;
                best_id = n.peer_id;
            }
        }
        return best_id;
    }

    unsigned count() const {
        FebruaryCrit::Guard g;
        unsigned n = 0;
        for (unsigned i = 0; i < FEBRUARY_WORLD_MODEL_SIZE; ++i) {
            if (nodes_[i].peer_id) ++n;
        }
        return n;
    }

    const DeviceNode* slot_at(unsigned i) const {
        return (i < FEBRUARY_WORLD_MODEL_SIZE) ? &nodes_[i] : nullptr;
    }

private:
    DeviceGraph() = default;

    DeviceNode* find_unlocked(uint32_t peer_id) {
        for (unsigned i = 0; i < FEBRUARY_WORLD_MODEL_SIZE; ++i) {
            if (nodes_[i].peer_id == peer_id) return &nodes_[i];
        }
        return nullptr;
    }
    const DeviceNode* find_unlocked(uint32_t peer_id) const {
        for (unsigned i = 0; i < FEBRUARY_WORLD_MODEL_SIZE; ++i) {
            if (nodes_[i].peer_id == peer_id) return &nodes_[i];
        }
        return nullptr;
    }

    DeviceNode* alloc_unlocked() {
        for (unsigned i = 0; i < FEBRUARY_WORLD_MODEL_SIZE; ++i) {
            if (nodes_[i].peer_id == 0) return &nodes_[i];
        }
        unsigned oldest = 0;
        for (unsigned i = 1; i < FEBRUARY_WORLD_MODEL_SIZE; ++i) {
            if (nodes_[i].last_seen_ms < nodes_[oldest].last_seen_ms) {
                oldest = i;
            }
        }
        nodes_[oldest] = DeviceNode{};
        return &nodes_[oldest];
    }

    DeviceNode nodes_[FEBRUARY_WORLD_MODEL_SIZE]{};
};

#else

enum class DeviceCap : uint16_t { None = 0, Light = 1, Speaker = 2, Display = 4, Sensory = 8 };
struct DeviceNode { uint32_t peer_id = 0; };

class DeviceGraph {
public:
    static DeviceGraph& instance() {
        static DeviceGraph g;
        return g;
    }
    void clear() {}
    DeviceNode* touch(uint32_t, const char*, uint32_t) { return nullptr; }
    void set_caps(uint32_t, uint16_t, uint32_t) {}
    void set_room(uint32_t, uint8_t, uint32_t) {}
    void set_battery(uint32_t, uint8_t, uint32_t) {}
    void set_trust(uint32_t, uint8_t, uint32_t) {}
    void note_tx(uint32_t, uint32_t, bool) {}
    void note_rx(uint32_t, uint32_t) {}
    void set_session_open(uint32_t, bool) {}
    DeviceNode* find(uint32_t) { return nullptr; }
    const DeviceNode* find(uint32_t) const { return nullptr; }
    uint32_t route(DeviceCap, uint8_t, uint8_t = 10) const { return 0; }
    unsigned count() const { return 0; }
};

#endif

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_WORLD_MODEL_HPP
