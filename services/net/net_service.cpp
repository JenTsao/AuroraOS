#include "net_service.hpp"
#include "syscall.hpp"

#ifdef AURORA_HOST_TEST
  #ifdef _WIN32
    #include <winsock2.h>
    typedef int socklen_t;
  #else
    #include <sys/socket.h>
    #include <sys/select.h>
    #include <sys/time.h>
    #include <arpa/inet.h>
  #endif
  
  // Stubs for host testing since we don't have lwIP available
  #define lwip_socket(domain, type, protocol) -1
  #define lwip_connect(fd, addr, addrlen) -1
  #define lwip_bind(fd, addr, addrlen) -1
  #define lwip_send(fd, data, size, flags) -1
  #define lwip_recv(fd, mem, len, flags) -1
  #define lwip_sendto(fd, data, size, flags, to, tolen) -1
  #define lwip_recvfrom(fd, mem, len, flags, from, fromlen) -1
  #define lwip_fcntl(fd, cmd, val) -1
  #define lwip_setsockopt(fd, level, optname, optval, optlen) -1
  #define lwip_getsockopt(fd, level, optname, optval, optlen) -1
  #define lwip_select(maxfdp1, readset, writeset, exceptset, timeout) -1
  #define lwip_close(fd) -1
  
  #define lwip_htons htons
  #define lwip_htonl htonl
  #define lwip_ntohs ntohs
  #define lwip_ntohl ntohl
#else
  #include "lwip/sockets.h"
  #include "lwip/inet.h"
#endif
#include <cstring>

namespace auroraos {
namespace net {

int g_net_service_ep = -1;

void NetServer::init() {
    // Initialize lwIP or other network stack components if needed
}

void NetServer::handle_socket(const NetRequest& req, NetReply& reply) {
    reply.status = lwip_socket(req.socket.domain, req.socket.type, req.socket.protocol);
}

void NetServer::handle_connect(const NetRequest& req, NetReply& reply) {
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(req.connect.port);
    addr.sin_addr.s_addr = lwip_htonl(req.connect.addr);
    
    reply.status = lwip_connect(req.connect.fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
}

void NetServer::handle_bind(const NetRequest& req, NetReply& reply) {
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(req.bind.port);
    addr.sin_addr.s_addr = lwip_htonl(req.bind.addr);
    
    reply.status = lwip_bind(req.bind.fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
}

void NetServer::handle_send(const NetRequest& req, NetReply& reply) {
    reply.status = lwip_send(req.send.fd, req.send.data, req.send.len, req.send.flags);
}

void NetServer::handle_recv(const NetRequest& req, NetReply& reply) {
    int max_len = req.recv.len;
    if (max_len > static_cast<int>(sizeof(reply.recv.data))) {
        max_len = sizeof(reply.recv.data);
    }
    
    reply.status = lwip_recv(req.recv.fd, reply.recv.data, max_len, req.recv.flags);
    if (reply.status > 0) {
        reply.recv.len = reply.status;
    }
}

void NetServer::handle_sendto(const NetRequest& req, NetReply& reply) {
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(req.sendto.port);
    addr.sin_addr.s_addr = lwip_htonl(req.sendto.addr);
    
    reply.status = lwip_sendto(req.sendto.fd, req.sendto.data, req.sendto.len, req.sendto.flags,
                               reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
}

void NetServer::handle_recvfrom(const NetRequest& req, NetReply& reply) {
    int max_len = req.recvfrom.len;
    if (max_len > static_cast<int>(sizeof(reply.recvfrom.data))) {
        max_len = sizeof(reply.recvfrom.data);
    }
    
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    
    reply.status = lwip_recvfrom(req.recvfrom.fd, reply.recvfrom.data, max_len, req.recvfrom.flags,
                                 reinterpret_cast<struct sockaddr*>(&addr), &addr_len);
                                 
    if (reply.status > 0) {
        reply.recvfrom.len = reply.status;
        reply.recvfrom.port = lwip_ntohs(addr.sin_port);
        reply.recvfrom.addr = lwip_ntohl(addr.sin_addr.s_addr);
    }
}

void NetServer::handle_fcntl(const NetRequest& req, NetReply& reply) {
    reply.status = lwip_fcntl(req.fcntl.fd, req.fcntl.cmd, req.fcntl.val);
}

void NetServer::handle_setsockopt(const NetRequest& req, NetReply& reply) {
    reply.status = lwip_setsockopt(req.setsockopt.fd, req.setsockopt.level, req.setsockopt.optname,
                                   req.setsockopt.optval, req.setsockopt.optlen);
}

void NetServer::handle_getsockopt(const NetRequest& req, NetReply& reply) {
    socklen_t optlen = req.getsockopt.optlen;
    if (optlen > sizeof(reply.getsockopt.optval)) {
        optlen = sizeof(reply.getsockopt.optval);
    }
    
    reply.status = lwip_getsockopt(req.getsockopt.fd, req.getsockopt.level, req.getsockopt.optname,
                                   reply.getsockopt.optval, &optlen);
    if (reply.status == 0) {
        reply.getsockopt.optlen = optlen;
    }
}

void NetServer::handle_select(const NetRequest& req, NetReply& reply) {
    fd_set readset;
    fd_set writeset;
    fd_set exceptset;
    
    FD_ZERO(&readset);
    FD_ZERO(&writeset);
    FD_ZERO(&exceptset);
    
    for (int i = 0; i < req.select.maxfdp1; ++i) {
        if (req.select.has_readset && (req.select.readset & (1 << i))) FD_SET(i, &readset);
        if (req.select.has_writeset && (req.select.writeset & (1 << i))) FD_SET(i, &writeset);
        if (req.select.has_exceptset && (req.select.exceptset & (1 << i))) FD_SET(i, &exceptset);
    }
    
    struct timeval tv;
    tv.tv_sec = req.select.timeout_ms / 1000;
    tv.tv_usec = (req.select.timeout_ms % 1000) * 1000;
    
    reply.status = lwip_select(req.select.maxfdp1, 
                               req.select.has_readset ? &readset : nullptr,
                               req.select.has_writeset ? &writeset : nullptr,
                               req.select.has_exceptset ? &exceptset : nullptr,
                               &tv);
                               
    if (reply.status > 0) {
        reply.select.readset = 0;
        reply.select.writeset = 0;
        reply.select.exceptset = 0;
        for (int i = 0; i < req.select.maxfdp1; ++i) {
            if (req.select.has_readset && FD_ISSET(i, &readset)) reply.select.readset |= (1 << i);
            if (req.select.has_writeset && FD_ISSET(i, &writeset)) reply.select.writeset |= (1 << i);
            if (req.select.has_exceptset && FD_ISSET(i, &exceptset)) reply.select.exceptset |= (1 << i);
        }
    }
}

void NetServer::handle_close(const NetRequest& req, NetReply& reply) {
    reply.status = lwip_close(req.close.fd);
}

void NetServer::process_request(const NetRequest& req, NetReply& reply) {
    reply.status = -1; // Default error
    switch (req.opcode) {
        case NetOpcode::Socket: handle_socket(req, reply); break;
        case NetOpcode::Connect: handle_connect(req, reply); break;
        case NetOpcode::Bind: handle_bind(req, reply); break;
        case NetOpcode::Send: handle_send(req, reply); break;
        case NetOpcode::Recv: handle_recv(req, reply); break;
        case NetOpcode::SendTo: handle_sendto(req, reply); break;
        case NetOpcode::RecvFrom: handle_recvfrom(req, reply); break;
        case NetOpcode::Fcntl: handle_fcntl(req, reply); break;
        case NetOpcode::Setsockopt: handle_setsockopt(req, reply); break;
        case NetOpcode::Getsockopt: handle_getsockopt(req, reply); break;
        case NetOpcode::Select: handle_select(req, reply); break;
        case NetOpcode::Close: handle_close(req, reply); break;
        default: break;
    }
}

[[noreturn]] void NetServer::run() {
    init();

    uint32_t ep_cap = g_net_service_ep;

    while (true) {
        struct {
            uint32_t msg_type;
            NetRequest req;
        } ipc_msg;
        
        uint32_t sender_id = 0;

        sys_ipc_receive(ep_cap, &ipc_msg, sizeof(ipc_msg), &sender_id);
        
        if (ipc_msg.msg_type == 2) { // Type 2 for Network requests
            NetReply reply;
            process_request(ipc_msg.req, reply);
            sys_ipc_reply(sender_id, &reply, sizeof(reply));
        }
    }
}

} // namespace net
} // namespace auroraos
