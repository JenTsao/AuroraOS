#include "vfs_client.hpp"
#include "syscall.hpp"
#include "../service_registry.hpp"
#include <string.h>

namespace auroraos {
namespace vfs {

VfsClient::DirectHandler VfsClient::direct_handler_ = nullptr;

VfsClient::VfsClient() : endpoint_cap_(-1) {}

void VfsClient::set_endpoint(int ep_cap) {
    endpoint_cap_ = ep_cap;
}

int VfsClient::get_endpoint() const {
    if (endpoint_cap_ >= 0) {
        return endpoint_cap_;
    }
    return services::ServiceRegistry::instance().get_service_endpoint(services::ServiceId::Vfs);
}

void VfsClient::set_direct_handler(DirectHandler handler) {
    direct_handler_ = handler;
}

static int do_vfs_call(int ep, const VfsRequest& req, VfsReply& reply, VfsClient::DirectHandler direct_handler) {
    if (direct_handler) {
        direct_handler(req, reply);
        return reply.status;
    }

    if (ep < 0) {
        ep = services::ServiceRegistry::instance().get_service_endpoint(services::ServiceId::Vfs);
    }

    if (ep < 0) {
        return -1;
    }

    struct {
        uint32_t msg_type;
        VfsRequest req;
    } ipc_msg;

    ipc_msg.msg_type = 1; // Type 1 for VFS requests
    ipc_msg.req = req;
    reply.status = -1;

    sys_ipc_call(static_cast<uint32_t>(ep), &ipc_msg, sizeof(ipc_msg), &reply, sizeof(reply));
    return reply.status;
}

int VfsClient::open(const char* path, int flags) {
    if (!path)
        return -1;

    VfsRequest req;
    req.opcode = VfsOpcode::Open;
    strncpy(req.open.path, path, sizeof(req.open.path) - 1);
    req.open.path[sizeof(req.open.path) - 1] = '\0';
    req.open.flags = flags;

    VfsReply reply;
    return do_vfs_call(get_endpoint(), req, reply, direct_handler_);
}

int VfsClient::read(int fd, void* buf, size_t len) {
    if (!buf || fd < 0)
        return -1;

    VfsRequest req;
    req.opcode = VfsOpcode::Read;
    req.fd = fd;
    req.read.len = len > VFS_IPC_BUFFER_SIZE ? VFS_IPC_BUFFER_SIZE : static_cast<int>(len);

    VfsReply reply;
    int ret = do_vfs_call(get_endpoint(), req, reply, direct_handler_);
    if (ret > 0) {
        memcpy(buf, reply.read.data, static_cast<size_t>(ret));
    }
    return ret;
}

int VfsClient::write(int fd, const void* buf, size_t len) {
    if (!buf || fd < 0)
        return -1;

    VfsRequest req;
    req.opcode = VfsOpcode::Write;
    req.fd = fd;
    req.write.len = len > VFS_IPC_BUFFER_SIZE ? VFS_IPC_BUFFER_SIZE : static_cast<int>(len);
    memcpy(req.write.data, buf, static_cast<size_t>(req.write.len));

    VfsReply reply;
    return do_vfs_call(get_endpoint(), req, reply, direct_handler_);
}

int VfsClient::lseek(int fd, int offset, int whence) {
    if (fd < 0)
        return -1;

    VfsRequest req;
    req.opcode = VfsOpcode::Lseek;
    req.fd = fd;
    req.lseek.offset = offset;
    req.lseek.whence = whence;

    VfsReply reply;
    return do_vfs_call(get_endpoint(), req, reply, direct_handler_);
}

int VfsClient::close(int fd) {
    if (fd < 0)
        return -1;

    VfsRequest req;
    req.opcode = VfsOpcode::Close;
    req.fd = fd;

    VfsReply reply;
    return do_vfs_call(get_endpoint(), req, reply, direct_handler_);
}

int VfsClient::ioctl(int fd, int request, void* arg) {
    if (fd < 0)
        return -1;

    VfsRequest req;
    req.opcode = VfsOpcode::Ioctl;
    req.fd = fd;
    req.ioctl.request = request;
    req.ioctl.arg = arg;

    VfsReply reply;
    return do_vfs_call(get_endpoint(), req, reply, direct_handler_);
}

int VfsClient::mount(const char* path, void* vnode_ptr) {
    if (!path || !vnode_ptr)
        return -1;

    VfsRequest req;
    req.opcode = VfsOpcode::Mount;
    strncpy(req.mount.path, path, sizeof(req.mount.path) - 1);
    req.mount.path[sizeof(req.mount.path) - 1] = '\0';
    req.mount.vnode_ptr = vnode_ptr;

    VfsReply reply;
    return do_vfs_call(get_endpoint(), req, reply, direct_handler_);
}

int VfsClient::unmount(const char* path) {
    if (!path)
        return -1;

    VfsRequest req;
    req.opcode = VfsOpcode::Unmount;
    strncpy(req.unmount.path, path, sizeof(req.unmount.path) - 1);
    req.unmount.path[sizeof(req.unmount.path) - 1] = '\0';

    VfsReply reply;
    return do_vfs_call(get_endpoint(), req, reply, direct_handler_);
}

int vfs_client_open(const char* path, int flags) {
    return VfsClient::instance().open(path, flags);
}

int vfs_client_read(int fd, void* buf, size_t len) {
    return VfsClient::instance().read(fd, buf, len);
}

int vfs_client_write(int fd, const void* buf, size_t len) {
    return VfsClient::instance().write(fd, buf, len);
}

int vfs_client_lseek(int fd, int offset, int whence) {
    return VfsClient::instance().lseek(fd, offset, whence);
}

int vfs_client_close(int fd) {
    return VfsClient::instance().close(fd);
}

} // namespace vfs
} // namespace auroraos
