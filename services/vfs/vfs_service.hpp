#ifndef VFS_SERVICE_HPP
#define VFS_SERVICE_HPP

#include "../../vfs/vfs.hpp" // VNode
#include "vfs_ipc.hpp"

namespace auroraos {
namespace vfs {

class alignas(8) VfsServer {
public:
#if defined(CONFIG_BOARD_NUCLEO_L031K6)
    static constexpr int MAX_MOUNT_POINTS = 4;
    static constexpr int MAX_OPEN_FILES = 4;
#else
    static constexpr int MAX_MOUNT_POINTS = 16;
    static constexpr int MAX_OPEN_FILES = 16;
#endif

    static VfsServer& instance() {
        static VfsServer server;
        return server;
    }

    void init();
    bool mount(const char* path, VNode* vnode);

    // Process a VFS request directly (used for unit testing and internal message dispatch)
    void process_request(const VfsRequest& req, VfsReply& reply);

    // Main loop for the VFS service task
    [[noreturn]] void run();

private:
    VfsServer() = default;

    struct alignas(8) MountPoint {
        char path[32];
        VNode* vnode;
    };

    struct alignas(8) FileDescriptor {
        VNode* vnode;
        int offset;
        bool used;
        void* priv;
        int ref_count;
    };

    MountPoint mounts_[MAX_MOUNT_POINTS]{};
    int mount_count_ = 0;
    FileDescriptor fd_table_[MAX_OPEN_FILES]{};

    // Helper functions
    bool strings_equal(const char* s1, const char* s2) const;
    void str_copy(char* dest, const char* src, int max_len);
    int starts_with(const char* prefix, const char* str) const;

    // Handlers
    void handle_open(const VfsRequest& req, VfsReply& reply);
    void handle_close(const VfsRequest& req, VfsReply& reply);
    void handle_read(const VfsRequest& req, VfsReply& reply);
    void handle_write(const VfsRequest& req, VfsReply& reply);
    void handle_ioctl(const VfsRequest& req, VfsReply& reply);
    void handle_lseek(const VfsRequest& req, VfsReply& reply);
};

// Entry point for the VFS service task
void vfs_service_main();

} // namespace vfs
} // namespace auroraos

#endif // VFS_SERVICE_HPP
