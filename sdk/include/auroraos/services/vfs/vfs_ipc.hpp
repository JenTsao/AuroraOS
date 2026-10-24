#ifndef VFS_IPC_HPP
#define VFS_IPC_HPP

#include <stdint.h>

namespace auroraos {
namespace vfs {

enum class VfsOpcode : uint32_t {
    Mount = 1,
    Open,
    Read,
    Write,
    Close,
    Ioctl,
    Lseek,
    Unmount
};

#if defined(CONFIG_BOARD_NUCLEO_L031K6)
static constexpr size_t VFS_IPC_BUFFER_SIZE = 128;
#else
static constexpr size_t VFS_IPC_BUFFER_SIZE = 1024;
#endif

// VfsRequest represents a file operation sent to the VFS Service
struct VfsRequest {
    VfsOpcode opcode;
    int fd;

    union {
        struct {
            char path[64];
            int flags;
        } open;

        struct {
            char path[64];
            void* vnode_ptr; // Used by kernel/system tasks for mounting
        } mount;

        struct {
            char path[64];
        } unmount;

        struct {
            int len;
        } read;

        struct {
            int len;
            char data[VFS_IPC_BUFFER_SIZE]; // Max write payload per message
        } write;

        struct {
            int request;
            void* arg;
        } ioctl;

        struct {
            int offset;
            int whence;
        } lseek;
    };
};

// VfsReply represents the result of a file operation returned to the client
struct VfsReply {
    int status; // Return code, bytes read/written, or fd

    union {
        struct {
            char data[VFS_IPC_BUFFER_SIZE]; // Payload for read responses
        } read;
    };
};

} // namespace vfs
} // namespace auroraos

#endif // VFS_IPC_HPP
