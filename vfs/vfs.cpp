#include "vfs.hpp"
#include "../services/vfs/vfs_ipc.hpp"
#include "../syscall/syscall.hpp"
#include <cstring>

using auroraos::vfs::VfsRequest;
using auroraos::vfs::VfsReply;
using auroraos::vfs::VfsOpcode;

namespace auroraos {
namespace vfs {
extern int g_vfs_service_ep; // Global defined in vfs_service.cpp (for now)
}
}

void VfsManager::init() {
    service_ep_cap_ = auroraos::vfs::g_vfs_service_ep;
}

void VfsManager::set_service_endpoint(int ep_cap) {
    service_ep_cap_ = ep_cap;
}

static int do_ipc_call(int ep_cap, const VfsRequest& req, VfsReply& reply) {
    if (ep_cap < 0) return -1;
    
    struct {
        uint32_t msg_type;
        VfsRequest req;
    } ipc_msg;
    
    ipc_msg.msg_type = 1; // VFS Request
    ipc_msg.req = req;
    reply.status = -1; // Default to error in case sys_ipc_call is a no-op (e.g. in test stubs)
    
    sys_ipc_call(ep_cap, &ipc_msg, sizeof(ipc_msg), &reply, sizeof(reply));
    return reply.status;
}

bool VfsManager::mount(const char* path, VNode* vnode) {
    if (service_ep_cap_ < 0) return false;
    VfsRequest req;
    req.opcode = VfsOpcode::Mount;
    std::strncpy(req.mount.path, path, sizeof(req.mount.path) - 1);
    req.mount.vnode_ptr = vnode;
    VfsReply reply;
    return do_ipc_call(service_ep_cap_, req, reply) == 0;
}

int VfsManager::open(const char* path, int flags) {
    if (service_ep_cap_ < 0) return -1;
    VfsRequest req;
    req.opcode = VfsOpcode::Open;
    std::strncpy(req.open.path, path, sizeof(req.open.path) - 1);
    req.open.flags = flags;
    VfsReply reply;
    return do_ipc_call(service_ep_cap_, req, reply);
}

int VfsManager::read(int fd, char* buf, int len) {
    if (service_ep_cap_ < 0) return -1;
    if (len > 1024) len = 1024; // Limit to IPC max payload for now
    VfsRequest req;
    req.opcode = VfsOpcode::Read;
    req.fd = fd;
    req.read.len = len;
    VfsReply reply;
    int bytes = do_ipc_call(service_ep_cap_, req, reply);
    if (bytes > 0) {
        std::memcpy(buf, reply.read.data, bytes);
    }
    return bytes;
}

int VfsManager::write(int fd, const char* buf, int len) {
    if (service_ep_cap_ < 0) return -1;
    if (len > 1024) len = 1024; // Limit to IPC max payload for now
    VfsRequest req;
    req.opcode = VfsOpcode::Write;
    req.fd = fd;
    req.write.len = len;
    std::memcpy(req.write.data, buf, len);
    VfsReply reply;
    return do_ipc_call(service_ep_cap_, req, reply);
}

int VfsManager::close(int fd) {
    if (service_ep_cap_ < 0) return -1;
    VfsRequest req;
    req.opcode = VfsOpcode::Close;
    req.fd = fd;
    VfsReply reply;
    return do_ipc_call(service_ep_cap_, req, reply);
}

int VfsManager::ioctl(int fd, int request, void* arg) {
    if (service_ep_cap_ < 0) return -1;
    VfsRequest req;
    req.opcode = VfsOpcode::Ioctl;
    req.fd = fd;
    req.ioctl.request = request;
    req.ioctl.arg = arg;
    VfsReply reply;
    return do_ipc_call(service_ep_cap_, req, reply);
}

int VfsManager::lseek(int fd, int offset, int whence) {
    if (service_ep_cap_ < 0) return -1;
    VfsRequest req;
    req.opcode = VfsOpcode::Lseek;
    req.fd = fd;
    req.lseek.offset = offset;
    req.lseek.whence = whence;
    VfsReply reply;
    return do_ipc_call(service_ep_cap_, req, reply);
}
