#ifndef NET_SERVICE_HPP
#define NET_SERVICE_HPP

#include "net_ipc.hpp"

namespace auroraos {
namespace net {

class NetServer {
public:
    static NetServer& instance() {
        static NetServer server;
        return server;
    }

    void init();
    
    // Process a Network request directly (used for unit testing and internal message dispatch)
    void process_request(const NetRequest& req, NetReply& reply);
    
    // Main loop for the Network service task
    [[noreturn]] void run();

private:
    NetServer() = default;
    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    void handle_socket(const NetRequest& req, NetReply& reply);
    void handle_connect(const NetRequest& req, NetReply& reply);
    void handle_bind(const NetRequest& req, NetReply& reply);
    void handle_send(const NetRequest& req, NetReply& reply);
    void handle_recv(const NetRequest& req, NetReply& reply);
    void handle_sendto(const NetRequest& req, NetReply& reply);
    void handle_recvfrom(const NetRequest& req, NetReply& reply);
    void handle_fcntl(const NetRequest& req, NetReply& reply);
    void handle_setsockopt(const NetRequest& req, NetReply& reply);
    void handle_getsockopt(const NetRequest& req, NetReply& reply);
    void handle_select(const NetRequest& req, NetReply& reply);
    void handle_close(const NetRequest& req, NetReply& reply);
};

// Global capability for the Network Service Endpoint
extern int g_net_service_ep;

} // namespace net
} // namespace auroraos

#endif // NET_SERVICE_HPP
