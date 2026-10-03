#include "vfs_service.hpp"
#include "syscall.hpp"
#include "../../kernel/core/device.hpp"
#include "../../kernel/task/task.hpp"

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

bool VfsServer::caller_authorized(uint32_t caller_id, const VNode* target, uint32_t required_right) const {
    if (!target)
        return false;

    // 进程内直连（同一地址空间，如 VfsManager::call_vfs 的 ep_cap<0 分支）：
    // 不存在跨信任域输入，沿用既有行为直接放行。
    if (caller_id == NO_IPC_CALLER)
        return true;

    TaskControlBlock* tcb = Scheduler::instance().get_task_by_id(caller_id);
    // fail-closed：查不到调用方 TCB 一律拒绝（绝不 fail-open）。
    if (!tcb)
        return false;

    // 内核特权任务旁路：ui_render_task()（apps/kernel.cpp）在启动期
    // open("/dev/touch0") 但不持有任何设备能力；不旁路会打断内核自身启动流程。
    if (tcb->privilege == 0u) // 0 = Kernel（见 task.hpp privilege 字段注释）
        return true;

    // 扫描调用方整个 cspace：VfsRequest 不携带槽位号，只能按「对象 + 权利」匹配。
    // Device 以双继承身份挂载（KernelObject 主基类 + VNode），能力 object 存的是
    // Device 主基类地址，必须经类型转换对齐到其 VNode 子对象后才能与 target 比较。
    for (uint32_t slot = 0; slot < auroraos::kernel::MAX_CSPACE_SLOTS; slot++) {
        const auroraos::kernel::Capability* cap = auroraos::kernel::CSpace::cap_lookup(tcb, slot);
        if (!cap || cap->type != auroraos::kernel::CapType::Device || !cap->object)
            continue;

        const Device* dev = static_cast<const Device*>(cap->object);
        if (static_cast<const VNode*>(dev) != target)
            continue;

        uint32_t held = 0;
        if (cap->rights.read)
            held |= auroraos::kernel::CAP_RIGHT_READ;
        if (cap->rights.write)
            held |= auroraos::kernel::CAP_RIGHT_WRITE;
        if (cap->rights.grant)
            held |= auroraos::kernel::CAP_RIGHT_GRANT;
        if ((held & required_right) == required_right)
            return true;
    }
    return false;
}

void VfsServer::handle_open(const VfsRequest& req, VfsReply& reply, uint32_t caller_id) {
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

    // 权限检查：调用方须持有指向 target 的设备能力，且权利覆盖 open 语义
    // （0=O_RDONLY→READ；1=O_WRONLY→WRITE；2=O_RDWR/未定义组合→READ|WRITE，
    // 值对齐 kernel/core/posix.hpp）。旁路/直连判定见 caller_authorized。
    const uint32_t acc = static_cast<uint32_t>(flags) & 0x3u;
    uint32_t need = auroraos::kernel::CAP_RIGHT_READ;
    if (acc == 1u) {
        need = auroraos::kernel::CAP_RIGHT_WRITE;
    } else if (acc == 2u || acc == 3u) {
        need = auroraos::kernel::CAP_RIGHT_READ | auroraos::kernel::CAP_RIGHT_WRITE;
    }
    if (!caller_authorized(caller_id, target, need)) {
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

void VfsServer::handle_read(const VfsRequest& req, VfsReply& reply, uint32_t caller_id) {
    int fd = req.fd;
    int len = req.read.len;
    if (fd < 0 || fd >= MAX_OPEN_FILES || !fd_table_[fd].used) {
        reply.status = -1;
        return;
    }

    // 操作时重验（而非仅 open 时）：能力撤销（cap_revoke）后立即生效；
    // fd 表全局共享，读侧也须确认当前调用方仍持有该 VNode 的读权。
    if (!caller_authorized(caller_id, fd_table_[fd].vnode, auroraos::kernel::CAP_RIGHT_READ)) {
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

void VfsServer::handle_write(const VfsRequest& req, VfsReply& reply, uint32_t caller_id) {
    int fd = req.fd;
    int len = req.write.len;
    if (fd < 0 || fd >= MAX_OPEN_FILES || !fd_table_[fd].used) {
        reply.status = -1;
        return;
    }

    // 操作时重验（而非仅 open 时）：能力撤销（cap_revoke）后立即生效；
    // fd 表全局共享，写侧也须确认当前调用方仍持有该 VNode 的写权。
    if (!caller_authorized(caller_id, fd_table_[fd].vnode, auroraos::kernel::CAP_RIGHT_WRITE)) {
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

    // 注意：req.ioctl.arg 是调用者提供的指针，具体语义由各 VNode 驱动定义。
    // 驱动实现必须在解引用前自行验证该指针（参见 syscall_validator）。
    VNode* vnode = fd_table_[fd].vnode;
    void* priv = fd_table_[fd].priv;
    reply.status = vnode->ioctl(req.ioctl.request, req.ioctl.arg, priv);
}

void VfsServer::handle_unmount(const VfsRequest& req, VfsReply& reply) {
    reply.status = unmount(req.unmount.path) ? 0 : -1;
}

void VfsServer::process_request(const VfsRequest& req, VfsReply& reply, uint32_t caller_id) {
    reply.status = -1; // Default error
    switch (req.opcode) {
    case VfsOpcode::Mount:
        // 安全边界：IPC 消息来自不可信调用者，禁止携带内核 VNode* 指针。
        // 挂载是内核特权操作，内核/系统代码必须直接调用 VfsServer::mount()。
        reply.status = -1;
        break;
    case VfsOpcode::Unmount:
        handle_unmount(req, reply);
        break;
    case VfsOpcode::Open:
        handle_open(req, reply, caller_id);
        break;
    case VfsOpcode::Read:
        handle_read(req, reply, caller_id);
        break;
    case VfsOpcode::Write:
        handle_write(req, reply, caller_id);
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

        // Wait for an IPC message via Syscall (blocking; void return by ABI contract)
        sys_ipc_receive(ep_cap, &ipc_msg, sizeof(ipc_msg), &sender_id);

        VfsReply reply;
        reply.status = -1; // 未知 opcode/消息类型一律返回错误
        if (ipc_msg.msg_type == 1) { // Type 1 for VFS requests
            // sender_id 即发送方 task id：跨信任域调用的权限判定依据。
            process_request(ipc_msg.req, reply, sender_id);
        }
        // 无论消息类型是否识别都必须回复，否则同步 IPC 客户端将永久阻塞
        sys_ipc_reply(sender_id, &reply, sizeof(reply));
    }
}

void vfs_service_main() {
    VfsServer::instance().run();
}

} // namespace vfs
} // namespace auroraos
