#ifndef VFS_CLIENT_HPP
#define VFS_CLIENT_HPP

#include "vfs_ipc.hpp"
#include <stddef.h>
#include <stdint.h>

namespace auroraos {
namespace vfs {

class VfsClient {
public:
    static VfsClient& instance() {
        static VfsClient client;
        return client;
    }

    void set_endpoint(int ep_cap);
    int get_endpoint() const;

    int open(const char* path, int flags);
    int read(int fd, void* buf, size_t len);
    int write(int fd, const void* buf, size_t len);
    int lseek(int fd, int offset, int whence);
    int close(int fd);
    int ioctl(int fd, int request, void* arg);

    // 挂载/卸载（特权服务或系统初始化使用）
    int mount(const char* path, void* vnode_ptr);
    int unmount(const char* path);

    // 直接分发器（用于单元测试和无 IPC 调度环境）
    using DirectHandler = void (*)(const VfsRequest& req, VfsReply& reply);
    static void set_direct_handler(DirectHandler handler);

private:
    VfsClient();
    int endpoint_cap_ = -1;
    static DirectHandler direct_handler_;
};

// C-style 便捷调用接口
int vfs_client_open(const char* path, int flags);
int vfs_client_read(int fd, void* buf, size_t len);
int vfs_client_write(int fd, const void* buf, size_t len);
int vfs_client_lseek(int fd, int offset, int whence);
int vfs_client_close(int fd);

} // namespace vfs
} // namespace auroraos

#endif // VFS_CLIENT_HPP
