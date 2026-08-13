#include "vfs.hpp"
#include "../services/vfs/vfs_ipc.hpp"
#include "../services/vfs/vfs_service.hpp"
#include "syscall.hpp"
#include <string.h>

using auroraos::vfs::VfsOpcode;
using auroraos::vfs::VfsReply;
using auroraos::vfs::VfsRequest;
using auroraos::vfs::VfsServer;

namespace auroraos {
namespace vfs {
extern int g_vfs_service_ep;
}
} // namespace auroraos

void VfsManager::init() {
    service_ep_cap_ = auroraos::vfs::g_vfs_service_ep;
    VfsServer::instance().init();
}

void VfsManager::set_service_endpoint(int ep_cap) {
    service_ep_cap_ = ep_cap;
}

// Static working buffers: VfsRequest/VfsReply are ~2KB each; putting them on the
// shell task stack (2KB) overflows and faults. Single client is fine for HIL.
static VfsRequest g_vfs_req;
static VfsReply g_vfs_reply;

static int call_vfs(int ep_cap) {
    g_vfs_reply.status = -1;
    if (ep_cap < 0) {
        VfsServer::instance().process_request(g_vfs_req, g_vfs_reply);
        return g_vfs_reply.status;
    }

    struct {
        uint32_t msg_type;
        VfsRequest req;
    } ipc_msg;

    ipc_msg.msg_type = 1;
    ipc_msg.req = g_vfs_req;
    sys_ipc_call(ep_cap, &ipc_msg, sizeof(ipc_msg), &g_vfs_reply, sizeof(g_vfs_reply));
    return g_vfs_reply.status;
}

bool VfsManager::mount(const char* path, VNode* vnode) {
    if (!path || !vnode)
        return false;
    if (VfsServer::instance().mount(path, vnode)) {
        return true;
    }
    if (service_ep_cap_ < 0)
        return false;
    memset(&g_vfs_req, 0, sizeof(g_vfs_req));
    g_vfs_req.opcode = VfsOpcode::Mount;
    strncpy(g_vfs_req.mount.path, path, sizeof(g_vfs_req.mount.path) - 1);
    g_vfs_req.mount.vnode_ptr = vnode;
    return call_vfs(service_ep_cap_) == 0;
}

int VfsManager::open(const char* path, int flags) {
    memset(&g_vfs_req, 0, sizeof(g_vfs_req));
    g_vfs_req.opcode = VfsOpcode::Open;
    strncpy(g_vfs_req.open.path, path, sizeof(g_vfs_req.open.path) - 1);
    g_vfs_req.open.flags = flags;
    return call_vfs(service_ep_cap_);
}

int VfsManager::read(int fd, char* buf, int len) {
    if (len > 1024)
        len = 1024;
    memset(&g_vfs_req, 0, sizeof(g_vfs_req));
    g_vfs_req.opcode = VfsOpcode::Read;
    g_vfs_req.fd = fd;
    g_vfs_req.read.len = len;
    int bytes = call_vfs(service_ep_cap_);
    if (bytes > 0) {
        memcpy(buf, g_vfs_reply.read.data, bytes);
    }
    return bytes;
}

int VfsManager::write(int fd, const char* buf, int len) {
    if (len > 1024)
        len = 1024;
    memset(&g_vfs_req, 0, sizeof(g_vfs_req));
    g_vfs_req.opcode = VfsOpcode::Write;
    g_vfs_req.fd = fd;
    g_vfs_req.write.len = len;
    memcpy(g_vfs_req.write.data, buf, len);
    return call_vfs(service_ep_cap_);
}

int VfsManager::close(int fd) {
    memset(&g_vfs_req, 0, sizeof(g_vfs_req));
    g_vfs_req.opcode = VfsOpcode::Close;
    g_vfs_req.fd = fd;
    return call_vfs(service_ep_cap_);
}

int VfsManager::ioctl(int fd, int request, void* arg) {
    memset(&g_vfs_req, 0, sizeof(g_vfs_req));
    g_vfs_req.opcode = VfsOpcode::Ioctl;
    g_vfs_req.fd = fd;
    g_vfs_req.ioctl.request = request;
    g_vfs_req.ioctl.arg = arg;
    return call_vfs(service_ep_cap_);
}

int VfsManager::lseek(int fd, int offset, int whence) {
    memset(&g_vfs_req, 0, sizeof(g_vfs_req));
    g_vfs_req.opcode = VfsOpcode::Lseek;
    g_vfs_req.fd = fd;
    g_vfs_req.lseek.offset = offset;
    g_vfs_req.lseek.whence = whence;
    return call_vfs(service_ep_cap_);
}
