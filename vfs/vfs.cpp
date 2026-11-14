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
// caller stack (2KB) overflows and faults. Access is serialized by g_vfs_mutex.
struct VfsIpcEnvelope {
    uint32_t msg_type;
    VfsRequest req;
};

static Mutex g_vfs_mutex;
static VfsIpcEnvelope g_vfs_ipc_msg;
static VfsRequest& g_vfs_req = g_vfs_ipc_msg.req;
static VfsReply g_vfs_reply;

static int call_vfs(int ep_cap) {
    g_vfs_reply.status = -1;
    if (ep_cap < 0) {
        VfsServer::instance().process_request(g_vfs_req, g_vfs_reply);
        return g_vfs_reply.status;
    }

    g_vfs_ipc_msg.msg_type = 1;
    sys_ipc_call(ep_cap, &g_vfs_ipc_msg, sizeof(g_vfs_ipc_msg), &g_vfs_reply, sizeof(g_vfs_reply));
    return g_vfs_reply.status;
}

bool VfsManager::mount(const char* path, VNode* vnode) {
    if (!path || !vnode)
        return false;
    // 挂载是内核特权操作，只允许直接调用服务端；
    // 禁止通过 IPC 消息传递内核 VNode* 指针（不可信调用者可伪造指针）。
    return VfsServer::instance().mount(path, vnode);
}

bool VfsManager::unmount(const char* path) {
    if (!path)
        return false;
    if (VfsServer::instance().unmount(path)) {
        return true;
    }
    if (service_ep_cap_ < 0)
        return false;
    LockGuard guard(g_vfs_mutex);
    memset(&g_vfs_req, 0, sizeof(g_vfs_req));
    g_vfs_req.opcode = VfsOpcode::Unmount;
    strncpy(g_vfs_req.unmount.path, path, sizeof(g_vfs_req.unmount.path) - 1);
    return call_vfs(service_ep_cap_) == 0;
}

int VfsManager::open(const char* path, int flags) {
    if (!path)
        return -1;
    LockGuard guard(g_vfs_mutex);
    memset(&g_vfs_req, 0, sizeof(g_vfs_req));
    g_vfs_req.opcode = VfsOpcode::Open;
    strncpy(g_vfs_req.open.path, path, sizeof(g_vfs_req.open.path) - 1);
    g_vfs_req.open.flags = flags;
    return call_vfs(service_ep_cap_);
}

int VfsManager::read(int fd, char* buf, int len) {
    if (!buf || len < 0)
        return -1;
    if (len == 0)
        return 0;
    constexpr int max_read = static_cast<int>(sizeof(g_vfs_reply.read.data));
    if (len > max_read)
        len = max_read;
    LockGuard guard(g_vfs_mutex);
    memset(&g_vfs_req, 0, sizeof(g_vfs_req));
    g_vfs_req.opcode = VfsOpcode::Read;
    g_vfs_req.fd = fd;
    g_vfs_req.read.len = len;
    int bytes = call_vfs(service_ep_cap_);
    if (bytes > 0) {
        if (bytes > len) {
            bytes = len;
        }
        memcpy(buf, g_vfs_reply.read.data, bytes);
    }
    return bytes;
}

int VfsManager::write(int fd, const char* buf, int len) {
    if (!buf || len < 0)
        return -1;
    if (len == 0)
        return 0;
    constexpr int max_write = static_cast<int>(sizeof(g_vfs_req.write.data));
    if (len > max_write)
        len = max_write;
    LockGuard guard(g_vfs_mutex);
    memset(&g_vfs_req, 0, sizeof(g_vfs_req));
    g_vfs_req.opcode = VfsOpcode::Write;
    g_vfs_req.fd = fd;
    g_vfs_req.write.len = len;
    memcpy(g_vfs_req.write.data, buf, len);
    return call_vfs(service_ep_cap_);
}

int VfsManager::close(int fd) {
    LockGuard guard(g_vfs_mutex);
    memset(&g_vfs_req, 0, sizeof(g_vfs_req));
    g_vfs_req.opcode = VfsOpcode::Close;
    g_vfs_req.fd = fd;
    return call_vfs(service_ep_cap_);
}

int VfsManager::ioctl(int fd, int request, void* arg) {
    LockGuard guard(g_vfs_mutex);
    memset(&g_vfs_req, 0, sizeof(g_vfs_req));
    g_vfs_req.opcode = VfsOpcode::Ioctl;
    g_vfs_req.fd = fd;
    g_vfs_req.ioctl.request = request;
    g_vfs_req.ioctl.arg = arg;
    return call_vfs(service_ep_cap_);
}

int VfsManager::lseek(int fd, int offset, int whence) {
    LockGuard guard(g_vfs_mutex);
    memset(&g_vfs_req, 0, sizeof(g_vfs_req));
    g_vfs_req.opcode = VfsOpcode::Lseek;
    g_vfs_req.fd = fd;
    g_vfs_req.lseek.offset = offset;
    g_vfs_req.lseek.whence = whence;
    return call_vfs(service_ep_cap_);
}
