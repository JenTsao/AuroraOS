/**
 * @file peer_table.hpp
 * @brief Fixed-slot peer state for cross-device SoftBus (Phase 2.2)
 *
 * Dual-writes into DeviceGraph when FEBRUARY_ENABLE_WORLD_MODEL=1.
 */
#ifndef AURORA_FEBRUARY_PEER_TABLE_HPP
#define AURORA_FEBRUARY_PEER_TABLE_HPP

#include "config.hpp"
#include "string_util.hpp"
#include "world_model.hpp"
#include <cstdint>

namespace aurora {
namespace february {

#if FEBRUARY_ENABLE_PEER_TABLE && FEBRUARY_ENABLE_SOFTBUS

constexpr unsigned kPeerTableSize = FEBRUARY_PEER_TABLE_SIZE;

struct PeerSlot {
    uint32_t peer_id        = 0;
    uint32_t last_seen_ms   = 0;
    uint32_t last_tx_ms     = 0;
    uint32_t last_rx_ms     = 0;
    uint16_t tx_ok          = 0;
    uint16_t tx_fail        = 0;
    uint16_t rx_ok          = 0;
    bool     session_open   = false;
    char     network_id[48] = {};
};

class PeerTable {
public:
    static PeerTable& instance() {
        static PeerTable t;
        return t;
    }

    void clear() {
        for (unsigned i = 0; i < kPeerTableSize; ++i) {
            slots_[i] = PeerSlot{};
        }
    }

    PeerSlot* touch(uint32_t peer_id, const char* network_id, uint32_t now_ms) {
        if (!peer_id) {
            return nullptr;
        }
        PeerSlot* s = find(peer_id);
        if (!s) {
            s = alloc();
        }
        if (!s) {
            return nullptr;
        }
        s->peer_id = peer_id;
        s->last_seen_ms = now_ms;
        if (network_id && network_id[0]) {
            copy_cstr(s->network_id, sizeof(s->network_id), network_id);
        }
#if FEBRUARY_ENABLE_WORLD_MODEL
        DeviceGraph::instance().touch(peer_id, network_id, now_ms);
#endif
        return s;
    }

    void note_tx(uint32_t peer_id, uint32_t now_ms, bool ok) {
        PeerSlot* s = find(peer_id);
        if (!s) {
            s = touch(peer_id, nullptr, now_ms);
        }
        if (!s) {
            return;
        }
        s->last_seen_ms = now_ms;
        s->last_tx_ms = now_ms;
        if (ok) {
            if (s->tx_ok < 0xFFFFu) {
                ++s->tx_ok;
            }
        } else if (s->tx_fail < 0xFFFFu) {
            ++s->tx_fail;
        }
#if FEBRUARY_ENABLE_WORLD_MODEL
        DeviceGraph::instance().note_tx(peer_id, now_ms, ok);
#endif
    }

    void note_rx(uint32_t peer_id, uint32_t now_ms) {
        PeerSlot* s = find(peer_id);
        if (!s) {
            s = touch(peer_id, nullptr, now_ms);
        }
        if (!s) {
            return;
        }
        s->last_seen_ms = now_ms;
        s->last_rx_ms = now_ms;
        if (s->rx_ok < 0xFFFFu) {
            ++s->rx_ok;
        }
#if FEBRUARY_ENABLE_WORLD_MODEL
        DeviceGraph::instance().note_rx(peer_id, now_ms);
#endif
    }

    void set_session_open(uint32_t peer_id, bool open) {
        PeerSlot* s = find(peer_id);
        if (s) {
            s->session_open = open;
        }
#if FEBRUARY_ENABLE_WORLD_MODEL
        DeviceGraph::instance().set_session_open(peer_id, open);
#endif
    }

    PeerSlot* find(uint32_t peer_id) {
        if (!peer_id) {
            return nullptr;
        }
        for (unsigned i = 0; i < kPeerTableSize; ++i) {
            if (slots_[i].peer_id == peer_id) {
                return &slots_[i];
            }
        }
        return nullptr;
    }

    const PeerSlot* find(uint32_t peer_id) const {
        return const_cast<PeerTable*>(this)->find(peer_id);
    }

    PeerSlot* find_by_network_id(const char* network_id) {
        if (!network_id || !network_id[0]) {
            return nullptr;
        }
        for (unsigned i = 0; i < kPeerTableSize; ++i) {
            if (slots_[i].peer_id && equals_ci(slots_[i].network_id, network_id)) {
                return &slots_[i];
            }
        }
        return nullptr;
    }

    const PeerSlot* find_by_network_id(const char* network_id) const {
        return const_cast<PeerTable*>(this)->find_by_network_id(network_id);
    }

    unsigned prune_stale(uint32_t now_ms, uint32_t timeout_ms) {
        unsigned pruned = 0;
        for (unsigned i = 0; i < kPeerTableSize; ++i) {
            if (slots_[i].peer_id != 0 && slots_[i].last_seen_ms != 0) {
                if (static_cast<uint32_t>(now_ms - slots_[i].last_seen_ms) > timeout_ms) {
                    slots_[i].session_open = false;
                    ++pruned;
                }
            }
        }
        return pruned;
    }

    unsigned count() const {
        unsigned n = 0;
        for (unsigned i = 0; i < kPeerTableSize; ++i) {
            if (slots_[i].peer_id) {
                ++n;
            }
        }
        return n;
    }

    const PeerSlot* slot_at(unsigned i) const {
        return (i < kPeerTableSize) ? &slots_[i] : nullptr;
    }

private:
    PeerTable() = default;

    PeerSlot* alloc() {
        for (unsigned i = 0; i < kPeerTableSize; ++i) {
            if (slots_[i].peer_id == 0) {
                return &slots_[i];
            }
        }
        unsigned oldest = 0;
        for (unsigned i = 1; i < kPeerTableSize; ++i) {
            if (slots_[i].last_seen_ms < slots_[oldest].last_seen_ms) {
                oldest = i;
            }
        }
        slots_[oldest] = PeerSlot{};
        return &slots_[oldest];
    }

    PeerSlot slots_[kPeerTableSize]{};
};

#else

struct PeerSlot {
    uint32_t peer_id = 0;
};

class PeerTable {
public:
    static PeerTable& instance() {
        static PeerTable t;
        return t;
    }
    void clear() {}
    PeerSlot* touch(uint32_t, const char*, uint32_t) { return nullptr; }
    void note_tx(uint32_t, uint32_t, bool) {}
    void note_rx(uint32_t, uint32_t) {}
    void set_session_open(uint32_t, bool) {}
    PeerSlot* find(uint32_t) { return nullptr; }
    const PeerSlot* find(uint32_t) const { return nullptr; }
    unsigned count() const { return 0; }
    const PeerSlot* slot_at(unsigned) const { return nullptr; }
};

#endif

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_PEER_TABLE_HPP
