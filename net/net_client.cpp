#include "net_client.hpp"
#include "../services/net/net_ipc.hpp"
#include "syscall.hpp"
#ifdef AURORA_HOST_TEST
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif
#define lwip_htons htons
#define lwip_htonl htonl
#define lwip_ntohs ntohs
#define lwip_ntohl ntohl
#else
#include "lwip/inet.h"
#endif
#include <string.h>

namespace auroraos {
namespace net {

static int g_net_service_client_ep = -1;

void set_net_service_endpoint(int ep_cap) {
    g_net_service_client_ep = ep_cap;
}

static int do_net_ipc_call(const NetRequest& req, NetReply& reply) {
    if (g_net_service_client_ep < 0)
        return -1;

    struct {
        uint32_t msg_type;
        NetRequest req;
    } ipc_msg;

    ipc_msg.msg_type = 2; // Network Request
    ipc_msg.req = req;
    reply.status = -1; // Default to error

    sys_ipc_call(g_net_service_client_ep, &ipc_msg, sizeof(ipc_msg), &reply, sizeof(reply));
    return reply.status;
}

int net_socket(int domain, int type, int protocol) {
    NetRequest req;
    req.opcode = NetOpcode::Socket;
    req.socket.domain = domain;
    req.socket.type = type;
    req.socket.protocol = protocol;
    NetReply reply;
    return do_net_ipc_call(req, reply);
}

int net_connect(int fd, const struct sockaddr* addr, socklen_t /*addrlen*/) {
    NetRequest req;
    req.opcode = NetOpcode::Connect;
    req.connect.fd = fd;
    const struct sockaddr_in* addr_in = reinterpret_cast<const struct sockaddr_in*>(addr);
    req.connect.port = lwip_ntohs(addr_in->sin_port);
    req.connect.addr = lwip_ntohl(addr_in->sin_addr.s_addr);
    NetReply reply;
    return do_net_ipc_call(req, reply);
}

int net_bind(int fd, const struct sockaddr* addr, socklen_t /*addrlen*/) {
    NetRequest req;
    req.opcode = NetOpcode::Bind;
    req.bind.fd = fd;
    const struct sockaddr_in* addr_in = reinterpret_cast<const struct sockaddr_in*>(addr);
    req.bind.port = lwip_ntohs(addr_in->sin_port);
    req.bind.addr = lwip_ntohl(addr_in->sin_addr.s_addr);
    NetReply reply;
    return do_net_ipc_call(req, reply);
}

int net_send(int fd, const void* data, size_t size, int flags) {
    NetRequest req;
    req.opcode = NetOpcode::Send;
    req.send.fd = fd;
    req.send.flags = flags;
    req.send.len = size > sizeof(req.send.data) ? sizeof(req.send.data) : size;
    memcpy(req.send.data, data, req.send.len);
    NetReply reply;
    return do_net_ipc_call(req, reply);
}

int net_recv(int fd, void* mem, size_t len, int flags) {
    NetRequest req;
    req.opcode = NetOpcode::Recv;
    req.recv.fd = fd;
    req.recv.flags = flags;
    req.recv.len = len;
    NetReply reply;
    int ret = do_net_ipc_call(req, reply);
    if (ret > 0) {
        memcpy(mem, reply.recv.data, ret);
    }
    return ret;
}

int net_sendto(int fd, const void* data, size_t size, int flags, const struct sockaddr* to, socklen_t /*tolen*/) {
    NetRequest req;
    req.opcode = NetOpcode::SendTo;
    req.sendto.fd = fd;
    req.sendto.flags = flags;
    const struct sockaddr_in* addr_in = reinterpret_cast<const struct sockaddr_in*>(to);
    req.sendto.port = lwip_ntohs(addr_in->sin_port);
    req.sendto.addr = lwip_ntohl(addr_in->sin_addr.s_addr);
    req.sendto.len = size > sizeof(req.sendto.data) ? sizeof(req.sendto.data) : size;
    memcpy(req.sendto.data, data, req.sendto.len);
    NetReply reply;
    return do_net_ipc_call(req, reply);
}

int net_recvfrom(int fd, void* mem, size_t len, int flags, struct sockaddr* from, socklen_t* /*fromlen*/) {
    NetRequest req;
    req.opcode = NetOpcode::RecvFrom;
    req.recvfrom.fd = fd;
    req.recvfrom.flags = flags;
    req.recvfrom.len = len;
    NetReply reply;
    int ret = do_net_ipc_call(req, reply);
    if (ret > 0) {
        memcpy(mem, reply.recvfrom.data, ret);
        if (from) {
            struct sockaddr_in* addr_in = reinterpret_cast<struct sockaddr_in*>(from);
            addr_in->sin_family = AF_INET;
            addr_in->sin_port = lwip_htons(reply.recvfrom.port);
            addr_in->sin_addr.s_addr = lwip_htonl(reply.recvfrom.addr);
        }
    }
    return ret;
}

int net_fcntl(int fd, int cmd, int val) {
    NetRequest req;
    req.opcode = NetOpcode::Fcntl;
    req.fcntl.fd = fd;
    req.fcntl.cmd = cmd;
    req.fcntl.val = val;
    NetReply reply;
    return do_net_ipc_call(req, reply);
}

int net_setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen) {
    NetRequest req;
    req.opcode = NetOpcode::Setsockopt;
    req.setsockopt.fd = fd;
    req.setsockopt.level = level;
    req.setsockopt.optname = optname;
    req.setsockopt.optlen = optlen > sizeof(req.setsockopt.optval) ? sizeof(req.setsockopt.optval) : optlen;
    memcpy(req.setsockopt.optval, optval, req.setsockopt.optlen);
    NetReply reply;
    return do_net_ipc_call(req, reply);
}

int net_getsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen) {
    NetRequest req;
    req.opcode = NetOpcode::Getsockopt;
    req.getsockopt.fd = fd;
    req.getsockopt.level = level;
    req.getsockopt.optname = optname;
    req.getsockopt.optlen = optlen ? *optlen : 0;
    NetReply reply;
    int ret = do_net_ipc_call(req, reply);
    if (ret == 0 && optval && optlen) {
        memcpy(optval, reply.getsockopt.optval, reply.getsockopt.optlen);
        *optlen = reply.getsockopt.optlen;
    }
    return ret;
}

int net_select(int maxfdp1, fd_set* readset, fd_set* writeset, fd_set* exceptset, struct timeval* timeout) {
    NetRequest req;
    req.opcode = NetOpcode::Select;
    req.select.maxfdp1 = maxfdp1;
    req.select.has_readset = (readset != nullptr);
    req.select.has_writeset = (writeset != nullptr);
    req.select.has_exceptset = (exceptset != nullptr);
    req.select.readset = 0;
    req.select.writeset = 0;
    req.select.exceptset = 0;

    for (int i = 0; i < maxfdp1; ++i) {
        if (readset && FD_ISSET(i, readset))
            req.select.readset |= (1 << i);
        if (writeset && FD_ISSET(i, writeset))
            req.select.writeset |= (1 << i);
        if (exceptset && FD_ISSET(i, exceptset))
            req.select.exceptset |= (1 << i);
    }

    if (timeout) {
        req.select.timeout_ms = timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
    } else {
        req.select.timeout_ms = 0xFFFFFFFF; // Infinite timeout representation
    }

    NetReply reply;
    int ret = do_net_ipc_call(req, reply);

    if (ret > 0) {
        if (readset) {
            FD_ZERO(readset);
            for (int i = 0; i < maxfdp1; ++i) {
                if (reply.select.readset & (1 << i))
                    FD_SET(i, readset);
            }
        }
        if (writeset) {
            FD_ZERO(writeset);
            for (int i = 0; i < maxfdp1; ++i) {
                if (reply.select.writeset & (1 << i))
                    FD_SET(i, writeset);
            }
        }
        if (exceptset) {
            FD_ZERO(exceptset);
            for (int i = 0; i < maxfdp1; ++i) {
                if (reply.select.exceptset & (1 << i))
                    FD_SET(i, exceptset);
            }
        }
    }

    return ret;
}

int net_close(int fd) {
    NetRequest req;
    req.opcode = NetOpcode::Close;
    req.close.fd = fd;
    NetReply reply;
    return do_net_ipc_call(req, reply);
}

} // namespace net
} // namespace auroraos
