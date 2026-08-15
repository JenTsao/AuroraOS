/**
 * @file softbus.hpp
 * @brief February SoftBus facade - inbox + transport + session map
 *
 * Cross-device path:
 *   publish_intent(peer, intent) -> pack -> transport.send_bytes (or loopback)
 *   transport RX -> unpack -> SoftBusMessage inbox -> FebruaryService::run_once
 *
 * Phase 2.2: PeerTable updates, close_session on disconnect, fail counters.
 */
#ifndef AURORA_FEBRUARY_SOFTBUS_HPP
#define AURORA_FEBRUARY_SOFTBUS_HPP

#include "config.hpp"
#include "types.hpp"
#include "softbus_stub.hpp"
#include "softbus_codec.hpp"
#include "softbus_transport.hpp"
#include "peer_table.hpp"
#include "log.hpp"
#include "string_util.hpp"

namespace aurora {
namespace february {

#if FEBRUARY_ENABLE_SOFTBUS

struct SoftBusSessionSlot {
    uint32_t         peer_id = 0;
    SoftBusSessionId session_id = kInvalidSession;
    bool             open = false;
    char             network_id[64] = {};
};

class SoftBus {
public:
    static SoftBus& instance() {
        static SoftBus bus;
        return bus;
    }

    void clear() {
        SoftBusStub::instance().clear();
        PeerTable::instance().clear();
        for (unsigned i = 0; i < FEBRUARY_SOFTBUS_MAX_SESSIONS; ++i) {
            slots_[i] = SoftBusSessionSlot{};
        }
        server_up_ = false;
        local_peer_id_ = 1;
        tx_count_ = rx_count_ = tx_fail_ = 0;
    }

    void bind_transport(const SoftBusTransportOps& ops) {
        ops_ = ops;
        rx_sink_.on_bytes = &SoftBus::static_on_bytes;
        rx_sink_.on_session_opened = &SoftBus::static_on_opened;
        rx_sink_.on_session_closed = &SoftBus::static_on_closed;
        rx_sink_.user = this;
        if (ops_.set_rx_sink) {
            ops_.set_rx_sink(&rx_sink_, ops_.user);
        }
    }

    const SoftBusTransportOps& transport() const { return ops_; }

    void set_local_peer_id(uint32_t id) { local_peer_id_ = id ? id : 1; }
    uint32_t local_peer_id() const { return local_peer_id_; }

    int start_server(const char* pkg = FEBRUARY_SOFTBUS_PKG,
                     const char* session = FEBRUARY_SOFTBUS_SESSION) {
        copy_cstr(pkg_, sizeof(pkg_), pkg ? pkg : FEBRUARY_SOFTBUS_PKG);
        copy_cstr(session_, sizeof(session_),
                  session ? session : FEBRUARY_SOFTBUS_SESSION);
        if (!ops_.create_server) {
            server_up_ = true;
            return 0;
        }
        const int rc = ops_.create_server(pkg_, session_, ops_.user);
        server_up_ = (rc == 0);
        FEBRUARY_LOG(server_up_ ? "softbus: server up" : "softbus: server fail");
        return rc;
    }

    int stop_server() {
        for (unsigned i = 0; i < FEBRUARY_SOFTBUS_MAX_SESSIONS; ++i) {
            if (slots_[i].open && slots_[i].session_id >= 0) {
                close_session_slot(slots_[i]);
            }
        }
        if (ops_.remove_server && server_up_) {
            ops_.remove_server(pkg_, session_, ops_.user);
        }
        server_up_ = false;
        return 0;
    }

    bool register_peer(uint32_t peer_id, const char* network_id,
                       uint32_t now_ms = 0) {
        if (!peer_id || !network_id) {
            return false;
        }
        SoftBusSessionSlot* slot = find_peer(peer_id);
        if (!slot) {
            slot = alloc_slot();
        }
        if (!slot) {
            return false;
        }
        slot->peer_id = peer_id;
        copy_cstr(slot->network_id, sizeof(slot->network_id), network_id);
        PeerTable::instance().touch(peer_id, network_id, now_ms);
        return true;
    }

    SoftBusSessionId ensure_session(uint32_t peer_id) {
        SoftBusSessionSlot* slot = find_peer(peer_id);
        if (!slot) {
            return kInvalidSession;
        }
        if (slot->open && slot->session_id >= 0) {
            return slot->session_id;
        }
        if (!ops_.open_session) {
            slot->session_id = static_cast<SoftBusSessionId>(peer_id);
            slot->open = true;
            PeerTable::instance().set_session_open(peer_id, true);
            return slot->session_id;
        }
        SoftBusSessionId sid = ops_.open_session(
            session_, session_, slot->network_id, ops_.user);
        if (sid >= 0) {
            slot->session_id = sid;
            slot->open = true;
            PeerTable::instance().set_session_open(peer_id, true);
        }
        return sid;
    }

    void close_peer(uint32_t peer_id) {
        for (unsigned i = 0; i < FEBRUARY_SOFTBUS_MAX_SESSIONS; ++i) {
            if (peer_id == 0 || slots_[i].peer_id == peer_id) {
                if (slots_[i].peer_id) {
                    close_session_slot(slots_[i]);
                }
            }
        }
    }

    bool publish_intent(uint32_t peer_id, const Intent& in, uint32_t now_ms,
                        bool loopback = false) {
        if (loopback || peer_id == 0) {
            // Only a genuine loopback (or "no peer" = local) request is
            // allowed to land directly in the local inbox. A real
            // outbound-to-peer command must never be locally enqueued
            // before/regardless of the transport actually delivering it.
            SoftBusStub::instance().publish(
                peer_id ? peer_id : local_peer_id_, in, now_ms);
            ++tx_count_;
            PeerTable::instance().note_tx(peer_id ? peer_id : local_peer_id_,
                                          now_ms, true);
            return true;
        }

        SoftBusSessionId sid = ensure_session(peer_id);
        if (sid < 0 || !ops_.send_bytes) {
            // No usable remote TX path: this is a failure, not a success,
            // so callers can distinguish it from a real delivery and retry.
            ++tx_fail_;
            PeerTable::instance().note_tx(peer_id, now_ms, false);
            FEBRUARY_LOG("softbus: no tx path");
            return false;
        }

        uint8_t frame[kSoftBusFrameMax];
        const unsigned n = softbus_pack_intent(in, local_peer_id_, now_ms,
                                               frame, sizeof(frame));
        if (n == 0) {
            ++tx_fail_;
            PeerTable::instance().note_tx(peer_id, now_ms, false);
            return false;
        }
        const int rc = ops_.send_bytes(sid, frame, n, ops_.user);
        if (rc != 0) {
            ++tx_fail_;
            PeerTable::instance().note_tx(peer_id, now_ms, false);
            FEBRUARY_LOG("softbus: send fail");
            SoftBusSessionSlot* slot = find_peer(peer_id);
            if (slot) {
                slot->open = false;
                PeerTable::instance().set_session_open(peer_id, false);
            }
            return false;
        }
        ++tx_count_;
        PeerTable::instance().note_tx(peer_id, now_ms, true);
        return true;
    }

    void on_bytes_received(SoftBusSessionId session_id, const uint8_t* data,
                           unsigned len) {
        Intent in;
        uint32_t peer = 0;
        uint32_t ts = 0;
        if (!softbus_unpack_intent(data, len, in, peer, ts)) {
            FEBRUARY_LOG("softbus: bad frame");
            return;
        }
        if (!peer) {
            peer = peer_for_session(session_id);
        }
        SoftBusStub::instance().publish(peer, in, ts);
        PeerTable::instance().note_rx(peer, ts);
        ++rx_count_;
        FEBRUARY_LOG("softbus: rx intent");
    }

    void on_session_opened(SoftBusSessionId session_id, int result) {
        if (result != 0) {
            return;
        }
        for (unsigned i = 0; i < FEBRUARY_SOFTBUS_MAX_SESSIONS; ++i) {
            if (slots_[i].session_id == session_id) {
                slots_[i].open = true;
                PeerTable::instance().set_session_open(slots_[i].peer_id, true);
            }
        }
    }

    void on_session_closed(SoftBusSessionId session_id) {
        for (unsigned i = 0; i < FEBRUARY_SOFTBUS_MAX_SESSIONS; ++i) {
            if (slots_[i].session_id == session_id) {
                slots_[i].open = false;
                slots_[i].session_id = kInvalidSession;
                PeerTable::instance().set_session_open(slots_[i].peer_id,
                                                       false);
            }
        }
    }

    unsigned pending() const { return SoftBusStub::instance().pending(); }
    bool pop(SoftBusMessage& out) { return SoftBusStub::instance().pop(out); }
    template <typename Fn>
    unsigned drain(unsigned max_n, Fn&& fn) {
        return SoftBusStub::instance().drain(max_n, static_cast<Fn&&>(fn));
    }

    uint32_t tx_count() const { return tx_count_; }
    uint32_t rx_count() const { return rx_count_; }
    uint32_t tx_fail() const { return tx_fail_; }
    bool server_up() const { return server_up_; }

private:
    SoftBus() {
        pkg_[0] = '\0';
        session_[0] = '\0';
        copy_cstr(pkg_, sizeof(pkg_), FEBRUARY_SOFTBUS_PKG);
        copy_cstr(session_, sizeof(session_), FEBRUARY_SOFTBUS_SESSION);
    }

    void close_session_slot(SoftBusSessionSlot& slot) {
        if (slot.open && slot.session_id >= 0 && ops_.close_session) {
            ops_.close_session(slot.session_id, ops_.user);
        }
        PeerTable::instance().set_session_open(slot.peer_id, false);
        slot.open = false;
        slot.session_id = kInvalidSession;
    }

    SoftBusSessionSlot* find_peer(uint32_t peer_id) {
        for (unsigned i = 0; i < FEBRUARY_SOFTBUS_MAX_SESSIONS; ++i) {
            if (slots_[i].peer_id == peer_id) {
                return &slots_[i];
            }
        }
        return nullptr;
    }

    SoftBusSessionSlot* alloc_slot() {
        for (unsigned i = 0; i < FEBRUARY_SOFTBUS_MAX_SESSIONS; ++i) {
            if (slots_[i].peer_id == 0) {
                return &slots_[i];
            }
        }
        return nullptr;
    }

    uint32_t peer_for_session(SoftBusSessionId sid) const {
        for (unsigned i = 0; i < FEBRUARY_SOFTBUS_MAX_SESSIONS; ++i) {
            if (slots_[i].session_id == sid) {
                return slots_[i].peer_id;
            }
        }
        return 0;
    }

    static void static_on_bytes(SoftBusSessionId sid, const uint8_t* data,
                                unsigned len, void* user) {
        static_cast<SoftBus*>(user)->on_bytes_received(sid, data, len);
    }
    static void static_on_opened(SoftBusSessionId sid, int result, void* user) {
        static_cast<SoftBus*>(user)->on_session_opened(sid, result);
    }
    static void static_on_closed(SoftBusSessionId sid, void* user) {
        static_cast<SoftBus*>(user)->on_session_closed(sid);
    }

    SoftBusTransportOps ops_{};
    SoftBusRxSink       rx_sink_{};
    SoftBusSessionSlot  slots_[FEBRUARY_SOFTBUS_MAX_SESSIONS]{};
    char                pkg_[48];
    char                session_[48];
    uint32_t            local_peer_id_ = 1;
    bool                server_up_ = false;
    uint32_t            tx_count_ = 0;
    uint32_t            rx_count_ = 0;
    uint32_t            tx_fail_ = 0;
};

#else  // !FEBRUARY_ENABLE_SOFTBUS

class SoftBus {
public:
    static SoftBus& instance() {
        static SoftBus bus;
        return bus;
    }
    void clear() {}
    void bind_transport(const SoftBusTransportOps&) {}
    int start_server(const char* = nullptr, const char* = nullptr) {
        return -1;
    }
    int stop_server() { return 0; }
    bool register_peer(uint32_t, const char*, uint32_t = 0) { return false; }
    SoftBusSessionId ensure_session(uint32_t) { return kInvalidSession; }
    void close_peer(uint32_t) {}
    bool publish_intent(uint32_t, const Intent&, uint32_t, bool = false) {
        return false;
    }
    void on_bytes_received(SoftBusSessionId, const uint8_t*, unsigned) {}
    unsigned pending() const { return 0; }
    template <typename Fn>
    unsigned drain(unsigned, Fn&&) {
        return 0;
    }
};

#endif  // FEBRUARY_ENABLE_SOFTBUS

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_SOFTBUS_HPP
