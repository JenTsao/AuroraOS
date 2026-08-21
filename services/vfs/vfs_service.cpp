#include "vfs_service.hpp"
#include "syscall.hpp"

namespace auroraos {
namespace vfs {

bool VfsServer::strings_equal(const char* s1, const char* s2) const {
    if (!s1 || !s2)
        return false;
    while (*s1 && *s2) {
        if (*s1 != *s2)
            return false;
        s1++;
        s2++;
    }
    return (*s1 == '\0' && *s2 == '\0');
}

void VfsServer::str_copy(char* dest, const char* src, int max_len) {
    int i = 0;
    while (*src && i < max_len - 1)
        dest[i++] = *src++;
    dest[i] = '\0';
}

int VfsServer::starts_with(const char* prefix, const char* str) const {
    if (!prefix || !str)
        return 0;
    int len = 0;
    while (*prefix) {
        if (*prefix != *str)
            return 0;
        prefix++;
        str++;
        len++;
    }
    return len;
}

void VfsServer::init() {
    mount_count_ = 0;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        fd_table_[i].used = false;
        fd_table_[i].priv = nullptr;
    }
}

bool VfsServer::mount(const char* path, VNode* vnode) {
    if (!path || !vnode)
        return false;
    int path_len = 0;
    bool has_traversal = false;
    for (const char* p = path; *p; p++, path_len++) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0') && (p == path || p[-1] == '/')) {
            has_traversal = true;
        }
        if (path_len >= 31)
            return false;
    }
    if (has_traversal)
        return false;
    if (mount_count_ >= MAX_MOUNT_POINTS)
        return false;
    str_copy(mounts_[mount_count_].path, path, sizeof(mounts_[0].path));
    mounts_[mount_count_].vnode = vnode;
    mount_count_++;
    return true;
}

bool VfsServer::unmount(const char* path) {
    if (!path)
        return false;
    for (int i = 0; i < mount_count_; i++) {
        if (strings_equal(mounts_[i].path, path)) {
            for (int j = i; j < mount_count_ - 1; j++) {
                mounts_[j] = mounts_[j + 1];
            }
            mount_count_--;
            return true;
        }
    }
    return false;
}

void VfsServer::handle_open(const VfsRequest& req, VfsReply& reply) {
    const char* path = req.open.path;
    int flags = req.open.flags;

    int path_len = 0;
    bool has_traversal = false;
    for (const char* p = path; *p; p++, path_len++) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0') && (p == path || p[-1] == '/')) {
            has_traversal = true;
        }
        if (path_len >= 63) {
            reply.status = -1;
            return;
        }
    }
    if (has_traversal) {
        reply.status = -1;
        return;
    }

    VNode* target = nullptr;
    int max_prefix_len = 0;

    for (int i = 0; i < mount_count_; i++) {
        int prefix_len = starts_with(mounts_[i].path, path);
        if (prefix_len > max_prefix_len) {
            if (path[prefix_len] == '\0' || path[prefix_len] == '/' || mounts_[i].path[prefix_len - 1] == '/') {
                max_prefix_len = prefix_len;
                target = mounts_[i].vnode;
            }
        }
    }

    if (!target) {
        reply.status = -1;
        return;
    }

    const char* relative_path = path + max_prefix_len;
    while (*relative_path == '/')
        relative_path++;
    if (*relative_path == '\0')
        relative_path = "/";

    for (int fd = 0; fd < MAX_OPEN_FILES; fd++) {
        if (!fd_table_[fd].used) {
            fd_table_[fd].used = true;
            fd_table_[fd].priv = nullptr;
            fd_table_[fd].ref_count = 0;

            if (target->open_file(relative_path, flags, &fd_table_[fd].priv) < 0) {
                fd_table_[fd].used = false;
                reply.status = -1;
                return;
            }

            fd_table_[fd].vnode = target;
            fd_table_[fd].offset = 0;
            reply.status = fd;
            return;
        }
    }
    reply.status = -1;
}

void VfsServer::handle_read(const VfsRequest& req, VfsReply& reply) {
    int fd = req.fd;
    int len = req.read.len;
    if (fd < 0 || fd >= MAX_OPEN_FILES || !fd_table_[fd].used) {
        reply.status = -1;
        return;
    }
    if (len < 0 || len > static_cast<int>(sizeof(reply.read.data)))
        len = sizeof(reply.read.data);

    VNode* vnode = fd_table_[fd].vnode;
    void* priv = fd_table_[fd].priv;
    int offset = fd_table_[fd].offset;

    int bytes = vnode->read(reply.read.data, len, offset, priv);
    if (bytes > 0) {
        fd_table_[fd].offset += bytes;
    }
    reply.status = bytes;
}

void VfsServer::handle_write(const VfsRequest& req, VfsReply& reply) {
    int fd = req.fd;
    int len = req.write.len;
    if (fd < 0 || fd >= MAX_OPEN_FILES || !fd_table_[fd].used) {
        reply.status = -1;
        return;
    }
    if (len < 0 || len > static_cast<int>(sizeof(req.write.data)))
        len = sizeof(req.write.data);

    VNode* vnode = fd_table_[fd].vnode;
    void* priv = fd_table_[fd].priv;
    int offset = fd_table_[fd].offset;

    int bytes = vnode->write(req.write.data, len, offset, priv);
    if (bytes > 0) {
        fd_table_[fd].offset += bytes;
    }
    reply.status = bytes;
}

void VfsServer::handle_lseek(const VfsRequest& req, VfsReply& reply) {
    int fd = req.fd;
    int offset = req.lseek.offset;
    int whence = req.lseek.whence;

    if (fd >= 0 && fd < MAX_OPEN_FILES && fd_table_[fd].used) {
        int new_offset = -1;
        if (whence == 0) { // SEEK_SET
            new_offset = offset;
        } else if (whence == 1) { // SEEK_CUR
            new_offset = fd_table_[fd].offset + offset;
        } else if (whence == 2) { // SEEK_END
            new_offset = fd_table_[fd].vnode->get_size(fd_table_[fd].priv) + offset;
        }

        if (new_offset >= 0) {
            fd_table_[fd].offset = new_offset;
            reply.status = new_offset;
            return;
        }
    }
    reply.status = -1;
}

void VfsServer::handle_close(const VfsRequest& req, VfsReply& reply) {
    int fd = req.fd;
    if (fd < 0 || fd >= MAX_OPEN_FILES || !fd_table_[fd].used) {
        reply.status = -1;
        return;
    }

    VNode* vnode = fd_table_[fd].vnode;
    void* priv = fd_table_[fd].priv;
    fd_table_[fd].used = false;
    fd_table_[fd].priv = nullptr;

    reply.status = vnode->close_file(priv);
}

void VfsServer::handle_ioctl(const VfsRequest& req, VfsReply& reply) {
    int fd = req.fd;
    if (fd < 0 || fd >= MAX_OPEN_FILES || !fd_table_[fd].used) {
        reply.status = -1;
        return;
    }

    VNode* vnode = fd_table_[fd].vnode;
    void* priv = fd_table_[fd].priv;
    reply.status = vnode->ioctl(req.ioctl.request, req.ioctl.arg, priv);
}

void VfsServer::handle_unmount(const VfsRequest& req, VfsReply& reply) {
    reply.status = unmount(req.unmount.path) ? 0 : -1;
}

void VfsServer::process_request(const VfsRequest& req, VfsReply& reply) {
    reply.status = -1; // Default error
    switch (req.opcode) {
    case VfsOpcode::Mount:
        reply.status = mount(req.mount.path, static_cast<VNode*>(req.mount.vnode_ptr)) ? 0 : -1;
        break;
    case VfsOpcode::Unmount:
        handle_unmount(req, reply);
        break;
    case VfsOpcode::Open:
        handle_open(req, reply);
        break;
    case VfsOpcode::Read:
        handle_read(req, reply);
        break;
    case VfsOpcode::Write:
        handle_write(req, reply);
        break;
    case VfsOpcode::Lseek:
        handle_lseek(req, reply);
        break;
    case VfsOpcode::Close:
        handle_close(req, reply);
        break;
    case VfsOpcode::Ioctl:
        handle_ioctl(req, reply);
        break;
    }
}

// Global variable to hold the VFS Service Endpoint capability ID for clients
int g_vfs_service_ep = -1;

[[noreturn]] void VfsServer::run() {
    init();

    // The VfsServer requires an Endpoint to receive IPC messages
    // The cap_id should be provided by the system loader. We use the global for now.
    uint32_t ep_cap = g_vfs_service_ep;

    while (true) {
        struct {
            uint32_t msg_type;
            VfsRequest req;
        } ipc_msg;

        uint32_t sender_id = 0;

        // Wait for an IPC message via Syscall
        sys_ipc_receive(ep_cap, &ipc_msg, sizeof(ipc_msg), &sender_id);

        if (ipc_msg.msg_type == 1) { // Type 1 for VFS requests
            VfsReply reply;
            process_request(ipc_msg.req, reply);
            // Send reply via Syscall
            sys_ipc_reply(sender_id, &reply, sizeof(reply));
        }
    }
}

void vfs_service_main() {
    VfsServer::instance().run();
}

} // namespace vfs
} // namespace auroraos
