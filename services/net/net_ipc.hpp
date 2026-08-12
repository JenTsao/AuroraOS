#ifndef NET_IPC_HPP
#define NET_IPC_HPP

#include <cstdint>
#include <cstddef>

namespace auroraos {
namespace net {

enum class NetOpcode : uint32_t {
    Socket = 1,
    Bind,
    Connect,
    Listen,
    Accept,
    Send,
    Recv,
    SendTo,
    RecvFrom,
    Setsockopt,
    Getsockopt,
    Fcntl,
    Select,
    Close
};

struct SocketReq {
    int domain;
    int type;
    int protocol;
};

struct ConnectReq {
    int fd;
    uint32_t addr; // IPv4 for now, simplify for milestone
    uint16_t port;
};

struct BindReq {
    int fd;
    uint32_t addr;
    uint16_t port;
};

struct SendReq {
    int fd;
    int flags;
    uint32_t len;
    char data[1024]; // Max IPC payload size limit for now
};

struct RecvReq {
    int fd;
    int flags;
    uint32_t len;
};

struct SendToReq {
    int fd;
    int flags;
    uint32_t addr;
    uint16_t port;
    uint32_t len;
    char data[1024];
};

struct RecvFromReq {
    int fd;
    int flags;
    uint32_t len;
};

struct FcntlReq {
    int fd;
    int cmd;
    int val;
};

struct SetsockoptReq {
    int fd;
    int level;
    int optname;
    uint32_t optlen;
    char optval[128];
};

struct GetsockoptReq {
    int fd;
    int level;
    int optname;
    uint32_t optlen;
};

struct SelectReq {
    int maxfdp1;
    bool has_readset;
    bool has_writeset;
    bool has_exceptset;
    uint32_t readset; // bitmask for simplified select
    uint32_t writeset;
    uint32_t exceptset;
    uint32_t timeout_ms;
};

struct CloseReq {
    int fd;
};

struct NetRequest {
    NetOpcode opcode;
    union {
        SocketReq socket;
        ConnectReq connect;
        BindReq bind;
        SendReq send;
        RecvReq recv;
        SendToReq sendto;
        RecvFromReq recvfrom;
        FcntlReq fcntl;
        SetsockoptReq setsockopt;
        GetsockoptReq getsockopt;
        SelectReq select;
        CloseReq close;
    };
};

struct NetReply {
    int status; // return code
    union {
        struct {
            uint32_t len;
            char data[1024];
        } recv;
        struct {
            uint32_t len;
            uint32_t addr;
            uint16_t port;
            char data[1024];
        } recvfrom;
        struct {
            uint32_t optlen;
            char optval[128];
        } getsockopt;
        struct {
            uint32_t readset;
            uint32_t writeset;
            uint32_t exceptset;
        } select;
    };
};

} // namespace net
} // namespace auroraos

#endif // NET_IPC_HPP
