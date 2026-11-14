#ifndef AURORA_LITTLEFS_VNODE_HPP
#define AURORA_LITTLEFS_VNODE_HPP

#include "vfs.hpp"
#include "photon_cache.hpp"
#include "mutex.hpp"
#include "../kernel/core/posix.hpp"
#include "../kernel/interrupt/timer.hpp"
#include <string.h>
// 引入第三方开源库接口 (假定项目附带于 3rdparty/littlefs/lfs.h)
extern "C" {
#include "lfs.h"
}

class LittleFsAdapter {
private:
    lfs_t lfs_;
    lfs_config cfg_;
    PhotonCacheLayer& cache_;
    mutable Mutex fs_mutex_;
    bool is_mounted_;

    // sync_timer_cb removed. 缓存落盘统一交由外部的 daemon_task 处理以避免生命周期竞态

    // ========================================================
    // 绑定给 LittleFS 的静态底层操作桥接回调
    // LittleFS 契约要求 read/prog/erase/sync 成功返回 0 (LFS_ERR_OK)，失败返回负错误码
    // ========================================================
    static int lfs_read_bridge(const struct lfs_config* c, lfs_block_t block, lfs_off_t off, void* buffer,
                               lfs_size_t size) {
        if (!c || !c->context || !buffer)
            return LFS_ERR_INVAL;
        if (size == 0)
            return LFS_ERR_OK;
        auto* cache = static_cast<PhotonCacheLayer*>(c->context);
        int res = cache->read(block, off, static_cast<uint8_t*>(buffer), size);
        return (res == static_cast<int>(size)) ? LFS_ERR_OK : LFS_ERR_IO;
    }

    static int lfs_prog_bridge(const struct lfs_config* c, lfs_block_t block, lfs_off_t off, const void* buffer,
                               lfs_size_t size) {
        if (!c || !c->context || !buffer)
            return LFS_ERR_INVAL;
        if (size == 0)
            return LFS_ERR_OK;
        auto* cache = static_cast<PhotonCacheLayer*>(c->context);
        int res = cache->write(block, off, static_cast<const uint8_t*>(buffer), size);
        return (res == static_cast<int>(size)) ? LFS_ERR_OK : LFS_ERR_IO;
    }

    static int lfs_erase_bridge(const struct lfs_config* c, lfs_block_t block) {
        if (!c || !c->context)
            return LFS_ERR_INVAL;
        auto* cache = static_cast<PhotonCacheLayer*>(c->context);
        return (cache->erase(block) == 0) ? LFS_ERR_OK : LFS_ERR_IO;
    }

    static int lfs_sync_bridge(const struct lfs_config* c) {
        if (!c || !c->context)
            return LFS_ERR_INVAL;
        auto* cache = static_cast<PhotonCacheLayer*>(c->context);
        return (cache->sync() == 0) ? LFS_ERR_OK : LFS_ERR_IO;
    }

public:
    LittleFsAdapter(PhotonCacheLayer& cache, uint32_t block_size = 4096, uint32_t block_count = 128)
        : cache_(cache), is_mounted_(false) {
        memset(&lfs_, 0, sizeof(lfs_));
        memset(&cfg_, 0, sizeof(cfg_));

        cfg_.context = &cache_;
        cfg_.read = lfs_read_bridge;
        cfg_.prog = lfs_prog_bridge;
        cfg_.erase = lfs_erase_bridge;
        cfg_.sync = lfs_sync_bridge;

        cfg_.read_size = 256;
        cfg_.prog_size = 256;
        cfg_.block_size = block_size;
        cfg_.block_count = block_count;
        cfg_.cache_size = 256;
        cfg_.lookahead_size = 16;
        cfg_.block_cycles = 500; // 开启动态磨损平衡
    }

    ~LittleFsAdapter() {
        unmount();
    }

    bool format() {
        LockGuard guard(fs_mutex_);
        if (is_mounted_) {
            lfs_unmount(&lfs_);
            is_mounted_ = false;
        }
        return lfs_format(&lfs_, &cfg_) == 0;
    }

    bool mount() {
        LockGuard guard(fs_mutex_);
        if (is_mounted_) {
            return true;
        }
        int err = lfs_mount(&lfs_, &cfg_);
        // P0 Fix: 挂载失败不应格式化，直接报告失败
        if (err != 0) {
            is_mounted_ = false;
        } else {
            is_mounted_ = true;
            // 不再自行创建定时器，由 kernel.cpp 的 system_daemon_task 统一调用 g_photon_cache.sync()
        }
        return is_mounted_;
    }

    void unmount() {
        LockGuard guard(fs_mutex_);
        if (is_mounted_) {
            lfs_unmount(&lfs_);
            is_mounted_ = false;
        }
    }

    bool is_mounted() const {
        LockGuard guard(fs_mutex_);
        return is_mounted_;
    }

    bool is_mounted_unlocked() const {
        return is_mounted_;
    }

    Mutex& get_mutex() const {
        return fs_mutex_;
    }

    lfs_t* get_lfs() {
        return &lfs_;
    }
};

// 继承自 VNode 的整个文件系统节点
class LittleFsVNode : public VNode {
private:
    LittleFsAdapter& fs_;

public:
    LittleFsVNode(LittleFsAdapter& fs) : fs_(fs) {}

    int open_file(const char* path, int flags, void** priv) override {
        if (!path || !priv)
            return -1;
        *priv = nullptr;

        int lfs_flags = 0;

        // 映射 O_ACCMODE 掩码
        if ((flags & 0x03) == O_RDWR) {
            lfs_flags |= LFS_O_RDWR;
        } else if ((flags & 0x03) == O_WRONLY) {
            lfs_flags |= LFS_O_WRONLY;
        } else {
            lfs_flags |= LFS_O_RDONLY; // Default to O_RDONLY (0)
        }

        // 映射创建/截断/追加标志
        if (flags & O_CREAT)
            lfs_flags |= LFS_O_CREAT;
        if (flags & O_TRUNC)
            lfs_flags |= LFS_O_TRUNC;
        if (flags & O_APPEND)
            lfs_flags |= LFS_O_APPEND;

        lfs_file_t* file = new lfs_file_t;
        if (!file)
            return -1;

        LockGuard guard(fs_.get_mutex());
        if (!fs_.is_mounted_unlocked()) {
            delete file;
            return -1;
        }

        int err = lfs_file_open(fs_.get_lfs(), file, path, lfs_flags);
        if (err == 0) {
            *priv = file;
            return 0;
        } else {
            delete file;
            return -1;
        }
    }

    int read(char* buf, int len, int offset, void* priv) override {
        if (!priv || !buf || offset < 0 || len < 0)
            return -1;
        if (len == 0)
            return 0;

        LockGuard guard(fs_.get_mutex());
        if (!fs_.is_mounted_unlocked())
            return -1;

        lfs_file_t* file = static_cast<lfs_file_t*>(priv);
        lfs_soff_t seek_res = lfs_file_seek(fs_.get_lfs(), file, static_cast<lfs_off_t>(offset), LFS_SEEK_SET);
        if (seek_res < 0)
            return -1;

        lfs_ssize_t res = lfs_file_read(fs_.get_lfs(), file, buf, static_cast<lfs_size_t>(len));
        return (res < 0) ? -1 : static_cast<int>(res);
    }

    int write(const char* buf, int len, int offset, void* priv) override {
        if (!priv || !buf || offset < 0 || len < 0)
            return -1;
        if (len == 0)
            return 0;

        LockGuard guard(fs_.get_mutex());
        if (!fs_.is_mounted_unlocked())
            return -1;

        lfs_file_t* file = static_cast<lfs_file_t*>(priv);
        lfs_soff_t seek_res = lfs_file_seek(fs_.get_lfs(), file, static_cast<lfs_off_t>(offset), LFS_SEEK_SET);
        if (seek_res < 0)
            return -1;

        lfs_ssize_t bytes = lfs_file_write(fs_.get_lfs(), file, buf, static_cast<lfs_size_t>(len));
        // P0 Fix: 去除每次 write 后的 lfs_file_sync，改为依赖定时器批量同步，延长 Flash 寿命
        return (bytes < 0) ? -1 : static_cast<int>(bytes);
    }

    int close_file(void* priv) override {
        if (!priv)
            return -1;

        lfs_file_t* file = static_cast<lfs_file_t*>(priv);
        int res = -1;
        {
            LockGuard guard(fs_.get_mutex());
            if (fs_.is_mounted_unlocked()) {
                int sync_res = lfs_file_sync(fs_.get_lfs(), file);
                int close_res = lfs_file_close(fs_.get_lfs(), file);
                res = (sync_res < 0 || close_res < 0) ? -1 : 0;
            }
        }
        delete file;
        return res;
    }

    int get_size(void* priv) const override {
        if (!priv)
            return 0;

        LockGuard guard(fs_.get_mutex());
        if (!fs_.is_mounted_unlocked())
            return 0;

        lfs_file_t* file = static_cast<lfs_file_t*>(priv);
        lfs_soff_t sz = lfs_file_size(fs_.get_lfs(), file);
        return (sz < 0) ? 0 : static_cast<int>(sz);
    }
};

#endif
