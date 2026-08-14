/**
 * @file softbus_oh_adapter.hpp
 * @brief Optional OpenHarmony-style SoftBus binding for February
 *
 * Does NOT include OH headers. Platform supplies function pointers matching
 * the classic session API:
 *   CreateSessionServer / RemoveSessionServer
 *   OpenSession / CloseSession / SendBytes
 *
 * Usage on a node that has real SoftBus:
 *
 *   #include "softbus_oh_adapter.hpp"
 *   aurora::february::OhSoftBusFns fns;
 *   fns.create_session_server = CreateSessionServer;
 *   fns.open_session = OpenSession;
 *   fns.send_bytes = SendBytes;
 *   // ...
 *   aurora::february::OhSoftBusAdapter::instance().bind(fns);
 *   SoftBus::instance().bind_transport(
 *       aurora::february::OhSoftBusAdapter::instance().ops());
 *   SoftBus::instance().start_server();
 *
 * Lite devices (passive only): leave open_session null; still RX via
 * on_bytes_received from the platform listener.
 */
#ifndef AURORA_FEBRUARY_SOFTBUS_OH_ADAPTER_HPP
#define AURORA_FEBRUARY_SOFTBUS_OH_ADAPTER_HPP

#include "softbus_transport.hpp"
#include "softbus.hpp"
#include "string_util.hpp"
#include <cstdint>

namespace aurora {
namespace february {

/**
 * Function signatures aligned with common OH SoftBus C API.
 * session open callback may be (sessionId, result) or (sessionId) on lite —
 * adapter normalizes to (sessionId, result).
 */
struct OhSoftBusFns {
    int (*create_session_server)(const char* pkg, const char* session_name,
                                 const void* listener) = nullptr;
    int (*remove_session_server)(const char* pkg,
                                 const char* session_name) = nullptr;
    /** Returns sessionId >= 0 or error. */
    int (*open_session)(const char* my_session, const char* peer_session,
                        const char* peer_network_id, const char* group_id,
                        const void* attr) = nullptr;
    void (*close_session)(int session_id) = nullptr;
    int (*send_bytes)(int session_id, const void* data, unsigned len) = nullptr;
    void* user = nullptr;
};

/**
 * Minimal listener blob stored for CreateSessionServer.
 * Platform may ignore and use its own listener that forwards to SoftBus.
 */
struct OhSessionListenerBridge {
    int (*OnSessionOpened)(int session_id, int result);
    void (*OnSessionClosed)(int session_id);
    void (*OnBytesReceived)(int session_id, const void* data, unsigned len);
    void (*OnMessageReceived)(int session_id, const void* data, unsigned len);
};

class OhSoftBusAdapter {
public:
    static OhSoftBusAdapter& instance() {
        static OhSoftBusAdapter a;
        return a;
    }

    void bind(const OhSoftBusFns& fns) {
        fns_ = fns;
        ops_.create_server = &OhSoftBusAdapter::tr_create_server;
        ops_.remove_server = &OhSoftBusAdapter::tr_remove_server;
        ops_.open_session = &OhSoftBusAdapter::tr_open_session;
        ops_.close_session = &OhSoftBusAdapter::tr_close_session;
        ops_.send_bytes = &OhSoftBusAdapter::tr_send_bytes;
        ops_.set_rx_sink = &OhSoftBusAdapter::tr_set_rx_sink;
        ops_.user = this;
    }

    const SoftBusTransportOps& ops() const { return ops_; }
    SoftBusTransportOps& ops() { return ops_; }

    /**
     * Call from platform OH listener when bytes arrive.
     * Forwards into SoftBus::on_bytes_received.
     */
    static void forward_bytes(int session_id, const void* data, unsigned len) {
        SoftBus::instance().on_bytes_received(
            static_cast<SoftBusSessionId>(session_id),
            static_cast<const uint8_t*>(data), len);
    }

    static int forward_opened(int session_id, int result) {
        SoftBus::instance().on_session_opened(
            static_cast<SoftBusSessionId>(session_id), result);
        return 0;
    }

    static void forward_closed(int session_id) {
        SoftBus::instance().on_session_closed(
            static_cast<SoftBusSessionId>(session_id));
    }

    /** Ready-made listener that bridges into SoftBus. */
    static const OhSessionListenerBridge& bridge_listener() {
        static const OhSessionListenerBridge L = {
            &OhSoftBusAdapter::forward_opened,
            &OhSoftBusAdapter::forward_closed,
            &OhSoftBusAdapter::bridge_on_bytes,
            nullptr,
        };
        return L;
    }

private:
    OhSoftBusAdapter() = default;

    static void bridge_on_bytes(int session_id, const void* data, unsigned len) {
        forward_bytes(session_id, data, len);
    }

    static int tr_create_server(const char* pkg, const char* session_name,
                                void* user) {
        auto* self = static_cast<OhSoftBusAdapter*>(user);
        if (!self->fns_.create_session_server) {
            return 0;
        }
        return self->fns_.create_session_server(
            pkg, session_name, &OhSoftBusAdapter::bridge_listener());
    }

    static int tr_remove_server(const char* pkg, const char* session_name,
                                void* user) {
        auto* self = static_cast<OhSoftBusAdapter*>(user);
        if (!self->fns_.remove_session_server) {
            return 0;
        }
        return self->fns_.remove_session_server(pkg, session_name);
    }

    static SoftBusSessionId tr_open_session(const char* local_session,
                                            const char* peer_session,
                                            const char* peer_network_id,
                                            void* user) {
        auto* self = static_cast<OhSoftBusAdapter*>(user);
        if (!self->fns_.open_session) {
            return kInvalidSession;
        }
        const int sid = self->fns_.open_session(
            local_session, peer_session, peer_network_id, "default", nullptr);
        return static_cast<SoftBusSessionId>(sid);
    }

    static int tr_close_session(SoftBusSessionId session_id, void* user) {
        auto* self = static_cast<OhSoftBusAdapter*>(user);
        if (self->fns_.close_session) {
            self->fns_.close_session(static_cast<int>(session_id));
        }
        return 0;
    }

    static int tr_send_bytes(SoftBusSessionId session_id, const void* data,
                             unsigned len, void* user) {
        auto* self = static_cast<OhSoftBusAdapter*>(user);
        if (!self->fns_.send_bytes) {
            return -1;
        }
        return self->fns_.send_bytes(static_cast<int>(session_id), data, len);
    }

    static void tr_set_rx_sink(const SoftBusRxSink* sink, void* user) {
        auto* self = static_cast<OhSoftBusAdapter*>(user);
        if (sink) {
            self->sink_ = *sink;
        }
        (void)user;
    }

    OhSoftBusFns        fns_{};
    SoftBusTransportOps ops_{};
    SoftBusRxSink       sink_{};
};

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_SOFTBUS_OH_ADAPTER_HPP
