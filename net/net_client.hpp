#ifndef NET_CLIENT_HPP
#define NET_CLIENT_HPP

#include <stdint.h>
#include <stddef.h>
#ifdef AURORA_HOST_TEST
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
// 主机侧必须自带 sockaddr_in / htons / INADDR_ANY 的声明：<sys/socket.h> 只提供
// sockaddr 家族骨架，套接字地址结构与字节序函数分别在 netinet/in.h、arpa/inet.h。
// 依赖其它头文件间接带入会在包含顺序变化时静默失配（曾导致 Linux 主机测试全线
// 报 "sockaddr_in 不完整类型"），故此处显式包含。
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
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
