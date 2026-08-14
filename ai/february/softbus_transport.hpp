/**
 * @file softbus_transport.hpp
 * @brief SoftBus transport ops — bind real stack or mock without changing SoftBus API
 *
 * February never calls OH / custom SoftBus headers directly. Platform code
 * fills SoftBusTransportOps once at boot:
 *
 *   SoftBusTransportOps ops{};
 *   ops.create_server = my_create;
 *   ops.send_bytes = my_send;
 *   ...
 *   SoftBus::instance().bind_transport(ops);
 *
 * Null function pointers = no remote I/O (local inbox still works).
 */
#ifndef AURORA_FEBRUARY_SOFTBUS_TRANSPORT_HPP
#define AURORA_FEBRUARY_SOFTBUS_TRANSPORT_HPP

#include <cstdint>

namespace aurora {
namespace february {

/** Opaque session handle from the underlying bus (OH sessionId, socket fd, …). */
using SoftBusSessionId = int32_t;

constexpr SoftBusSessionId kInvalidSession = -1;

/**
 * Transport callbacks invoked by SoftBus when the platform reports activity.
 * SoftBus registers these with the platform via ops.set_rx_sink if provided.
 */
struct SoftBusRxSink {
    /** Bytes received on a session; SoftBus unpacks Intent and enqueues. */
    void (*on_bytes)(SoftBusSessionId session_id, const uint8_t* data,
                     unsigned len, void* user) = nullptr;
    void (*on_session_opened)(SoftBusSessionId session_id, int result,
                              void* user) = nullptr;
    void (*on_session_closed)(SoftBusSessionId session_id, void* user) = nullptr;
    void* user = nullptr;
};

/**
 * Platform-supplied operations. All optional except send_bytes for TX path.
 * Return 0 on success, negative on error (OH SoftBus style).
 */
struct SoftBusTransportOps {
    /**
     * Register local session server (e.g. CreateSessionServer).
     * pkg / session_name are C strings owned by caller for the call duration.
     */
    int (*create_server)(const char* pkg, const char* session_name,
                         void* user) = nullptr;

    int (*remove_server)(const char* pkg, const char* session_name,
                         void* user) = nullptr;

    /**
     * Open session to peer. peer_network_id is SoftBus networkId string.
     * Returns session id (>=0) or negative error.
     */
    SoftBusSessionId (*open_session)(const char* local_session,
                                     const char* peer_session,
                                     const char* peer_network_id,
                                     void* user) = nullptr;

    int (*close_session)(SoftBusSessionId session_id, void* user) = nullptr;

    /** Reliable byte send on an open session. */
    int (*send_bytes)(SoftBusSessionId session_id, const void* data,
                      unsigned len, void* user) = nullptr;

    /**
     * Optional: platform installs SoftBusRxSink so RX can call back into
     * February without polling. If null, platform must call
     * SoftBus::on_bytes_received() from its own listener.
     */
    void (*set_rx_sink)(const SoftBusRxSink* sink, void* user) = nullptr;

    void* user = nullptr;
};

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_SOFTBUS_TRANSPORT_HPP
