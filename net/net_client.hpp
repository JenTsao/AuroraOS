#ifndef NET_CLIENT_HPP
#define NET_CLIENT_HPP

#include <stdint.h>
#include <stddef.h>
#ifdef AURORA_HOST_TEST
  #ifdef _WIN32
    #include <winsock2.h>
    typedef int socklen_t;
  #else
    #include <sys/socket.h>
    #include <sys/select.h>
    #include <sys/time.h>
  #endif
#else
  #include "lwip/sockets.h"
#endif

namespace auroraos {
namespace net {

// Set the endpoint capability ID for the NetService
void set_net_service_endpoint(int ep_cap);

int net_socket(int domain, int type, int protocol);
int net_connect(int fd, const struct sockaddr* addr, socklen_t addrlen);
int net_bind(int fd, const struct sockaddr* addr, socklen_t addrlen);
int net_send(int fd, const void* data, size_t size, int flags);
int net_recv(int fd, void* mem, size_t len, int flags);
int net_sendto(int fd, const void* data, size_t size, int flags, const struct sockaddr* to, socklen_t tolen);
int net_recvfrom(int fd, void* mem, size_t len, int flags, struct sockaddr* from, socklen_t* fromlen);
int net_fcntl(int fd, int cmd, int val);
int net_setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen);
int net_getsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen);
int net_select(int maxfdp1, fd_set* readset, fd_set* writeset, fd_set* exceptset, struct timeval* timeout);
int net_close(int fd);

} // namespace net
} // namespace auroraos

#endif // NET_CLIENT_HPP
