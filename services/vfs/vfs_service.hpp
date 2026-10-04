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
    bool unmount(const char* path);

    // 标记"进程内直接调用"（未经 VFS IPC 服务端）的 sender_id 哨兵值。
    // 取 UINT32_MAX：调度器 task id 上限为 MAX_TASKS(16)，不可能与真实
    // sender_id 碰撞，故可安全用作"无 IPC 发送方"标记。
    static constexpr uint32_t NO_IPC_CALLER = 0xFFFFFFFFu;

    // Process a VFS request directly (used for unit testing and internal message dispatch)
    //
    // caller_id 语义与 VFS 服务主循环 sys_ipc_receive 填入的 sender_id 一致，
    // 即**发送方 task id**（非 badge）。缺省为 NO_IPC_CALLER，表示调用方是
    // 与本服务处于同一地址空间的内部代码（VfsManager::call_vfs 的 ep_cap<0
    // 直连分支），此时不存在跨信任域的输入，沿用既有行为直接放行。
    void process_request(const VfsRequest& req, VfsReply& reply,
                         uint32_t caller_id = NO_IPC_CALLER);

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

    // capability 授权判定：确认 caller_id 对应的任务是否持有指向 target、
    // 且带 required_right 位的能力。判据风格对齐 kernel/core/device.cpp:228
    // 的 device_read()：cap_lookup -> 校验 type -> 校验 object 指向 -> 按位rights。
    //
    // fail-closed：查不到调用方 TCB 一律拒绝（绝不 fail-open）。
    // 内核特权任务旁路：ui_render_task() 直接 open("/dev/touch0") 读取触控
    // 数据（apps/kernel.cpp），其 TCB privilege 为 Kernel 且不持有任何设备
    // 能力；若不旁路会打断内核自身启动流程。
    bool caller_authorized(uint32_t caller_id, const VNode* target,
                           uint32_t required_right) const;

    // Handlers
    void handle_open(const VfsRequest& req, VfsReply& reply, uint32_t caller_id);
    void handle_close(const VfsRequest& req, VfsReply& reply);
    void handle_read(const VfsRequest& req, VfsReply& reply, uint32_t caller_id);
    void handle_write(const VfsRequest& req, VfsReply& reply, uint32_t caller_id);
    void handle_ioctl(const VfsRequest& req, VfsReply& reply);
    void handle_lseek(const VfsRequest& req, VfsReply& reply);
    void handle_unmount(const VfsRequest& req, VfsReply& reply);
};

// Entry point for the VFS service task
void vfs_service_main();

} // namespace vfs
} // namespace auroraos

#endif // VFS_SERVICE_HPP
