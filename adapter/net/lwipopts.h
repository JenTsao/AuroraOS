#ifndef LWIPOPTS_H
#define LWIPOPTS_H

// 1. Core Features
#define NO_SYS 0 // We have an OS!
#define LWIP_IPV4 1
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_SOCKET 1
#define LWIP_NETCONN 1
#define LWIP_ICMP 1 // For PING
#define LWIP_ARP 1
#define LWIP_DHCP 1
#define LWIP_IGMP 1

// 2. Memory configurations
#define MEM_ALIGNMENT 4
#define MEM_SIZE (2 * 1024)
#define MEMP_NUM_PBUF 4
#define MEMP_NUM_UDP_PCB 2
#define MEMP_NUM_TCP_PCB 2
#define MEMP_NUM_TCP_PCB_LISTEN 2
#define MEMP_NUM_TCP_SEG 4
#define PBUF_POOL_SIZE 2
#define PBUF_POOL_BUFSIZE 512

// 3. Thread / OSAL configuration
#define TCPIP_THREAD_NAME "tcpip"
#define TCPIP_THREAD_STACKSIZE 2048
#define TCPIP_THREAD_PRIO 1
#define TCPIP_MBOX_SIZE 16

#define DEFAULT_THREAD_STACKSIZE 2048
#define DEFAULT_THREAD_PRIO 1
#define DEFAULT_RAW_RECVMBOX_SIZE 16
#define DEFAULT_UDP_RECVMBOX_SIZE 16
#define DEFAULT_TCP_RECVMBOX_SIZE 16
#define DEFAULT_ACCEPTMBOX_SIZE 16

// 4. APIs
// Disable BSD socket compat macros (connect, read, write, etc.) — they collide
// with C++ method names (e.g. WifiDriver::connect). Use lwip_* prefixed calls.
#define LWIP_COMPAT_SOCKETS 0
#define LWIP_POSIX_SOCKETS_IO_NAMES 0
#define LWIP_PROVIDE_ERRNO 0
#define LWIP_NO_CTYPE 1
#define LWIP_TCPIP_CORE_LOCKING 1
#define LWIP_TIMEVAL_PRIVATE 0

#endif // LWIPOPTS_H
