#ifndef VFS_HPP
#define VFS_HPP

#include "mutex.hpp"

class VNode {
public:
    virtual ~VNode() = default;
    // 增加 open_file, 传递 flags 和 priv 指针
    virtual int open_file(const char* /*path*/, int /*flags*/, void** /*priv*/) { return 0; }
    virtual int close_file(void* /*priv*/) { return 0; }
    // 增加 offset 参数
    virtual int read(char* /*buf*/, int /*len*/, int /*offset*/, void* /*priv*/) { return -1; }
    virtual int write(const char* /*buf*/, int /*len*/, int /*offset*/, void* /*priv*/) { return -1; }
    virtual int ioctl(int /*request*/, void* /*arg*/, void* /*priv*/) { return -1; }
    virtual int get_size(void* /*priv*/) const { return 0; }

    // 【新增】引用计数，保护 VNode 本身在 I/O 期间不被释放（防止悬空指针）
    virtual void add_ref() { ref_count_++; }
    virtual void release_ref() { ref_count_--; }
    int get_ref() const { return ref_count_; }

protected:
    int ref_count_{0};
};

struct MountPoint {
    char path[32];
    VNode* vnode;
};

// 真正的文件描述符：记录打开的文件以及当前的读写位置
struct FileDescriptor {
    VNode* vnode;
    int offset;
    bool used;
    void* priv;
    int ref_count;
};

class VfsManager {
public:
    static VfsManager& instance() {
        static VfsManager vfs;
        return vfs;
    }

    void init();
    bool mount(const char* path, VNode* vnode);
    int open(const char* path, int flags = 0);
    int read(int fd, char* buf, int len);
    int write(int fd, const char* buf, int len);
    int close(int fd);
    int ioctl(int fd, int request, void* arg);
    
    // 【新增】系统调用：移动文件读写游标
    int lseek(int fd, int offset, int whence);

private:
    VfsManager() = default;
    VfsManager(const VfsManager&) = delete;
    VfsManager& operator=(const VfsManager&) = delete;

    static constexpr int MAX_MOUNT_POINTS = 8;
    static constexpr int MAX_OPEN_FILES = 16;

    MountPoint mounts_[MAX_MOUNT_POINTS]{};
    int mount_count_ = 0;

    // 升级为完整的文件描述符表
    FileDescriptor fd_table_[MAX_OPEN_FILES]{};
    Mutex vfs_mutex_;

    bool strings_equal(const char* s1, const char* s2) const;
    void str_copy(char* dest, const char* src, int max_len);
};

#endif
