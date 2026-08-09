#ifndef AURORA_LITTLEFS_VNODE_HPP
#define AURORA_LITTLEFS_VNODE_HPP

#include "vfs.hpp"
#include "photon_cache.hpp"
#include "../kernel/posix.hpp"
#include "../kernel/timer.hpp"
// 引入第三方开源库接口 (假定项目附带于 3rdparty/littlefs/lfs.h)
extern "C" {
#include "lfs.h"
}

class LittleFsAdapter {
private:
    lfs_t        lfs_;
    lfs_config   cfg_;
    PhotonCacheLayer& cache_;
    bool         is_mounted_;
    // sync_timer_cb removed. 缓存落盘统一交由外部的 daemon_task 处理以避免生命周期竞态

    // ========================================================
    // 绑定给 LittleFS 的静态底层操作桥接回调
    // ========================================================
    static int lfs_read_bridge(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size) {
        auto* cache = static_cast<PhotonCacheLayer*>(c->context);
        return cache->read(block, off, static_cast<uint8_t*>(buffer), size);
    }

    static int lfs_prog_bridge(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size) {
        auto* cache = static_cast<PhotonCacheLayer*>(c->context);
        return cache->write(block, off, static_cast<const uint8_t*>(buffer), size);
    }

    static int lfs_erase_bridge(const struct lfs_config *c, lfs_block_t block) {
        auto* cache = static_cast<PhotonCacheLayer*>(c->context);
        return cache->erase(block);
    }

    static int lfs_sync_bridge(const struct lfs_config *c) {
        auto* cache = static_cast<PhotonCacheLayer*>(c->context);
        return cache->sync();
    }

public:
    LittleFsAdapter(PhotonCacheLayer& cache, uint32_t block_size = 4096, uint32_t block_count = 128) 
        : cache_(cache), is_mounted_(false) {
        
        cfg_.context     = &cache_;
        cfg_.read        = lfs_read_bridge;
        cfg_.prog        = lfs_prog_bridge;
        cfg_.erase       = lfs_erase_bridge;
        cfg_.sync        = lfs_sync_bridge;
        
        cfg_.read_size   = 256;
        cfg_.prog_size   = 256;
        cfg_.block_size  = block_size;
        cfg_.block_count = block_count;
        cfg_.cache_size  = 256;
        cfg_.lookahead_size = 16;
        cfg_.block_cycles = 500; // 开启动态磨损平衡
        
        // Zero out remaining fields (e.g. name_max, file_max, attr_max, etc)
        // LittleFS expects these to be set or 0 to use defaults.
        cfg_.name_max = 0;
        cfg_.file_max = 0;
        cfg_.attr_max = 0;
        cfg_.metadata_max = 0;
    }

    ~LittleFsAdapter() {
        // 定时器已移除，无竞态风险
    }

    bool mount() {
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

    lfs_t* get_lfs() { return &lfs_; }
};

// 继承自 VNode 的整个文件系统节点
class LittleFsVNode : public VNode {
private:
    LittleFsAdapter& fs_;

public:
    LittleFsVNode(LittleFsAdapter& fs) : fs_(fs) {}

    int open_file(const char* path, int flags, void** priv) override {
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
        if (flags & O_CREAT)  lfs_flags |= LFS_O_CREAT;
        if (flags & O_TRUNC)  lfs_flags |= LFS_O_TRUNC;
        if (flags & O_APPEND) lfs_flags |= LFS_O_APPEND;

        lfs_file_t* file = new lfs_file_t;
        if (!file) return -1;

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
        if (!priv) return -1;
        lfs_file_t* file = static_cast<lfs_file_t*>(priv);
        lfs_file_seek(fs_.get_lfs(), file, offset, LFS_SEEK_SET);
        return lfs_file_read(fs_.get_lfs(), file, buf, len);
    }

    int write(const char* buf, int len, int offset, void* priv) override {
        if (!priv) return -1;
        lfs_file_t* file = static_cast<lfs_file_t*>(priv);
        lfs_file_seek(fs_.get_lfs(), file, offset, LFS_SEEK_SET);
        int bytes = lfs_file_write(fs_.get_lfs(), file, buf, len);
        // P0 Fix: 去除每次 write 后的 lfs_file_sync，改为依赖定时器批量同步，延长 Flash 寿命
        return bytes;
    }

    int close_file(void* priv) override {
        if (!priv) return -1;
        lfs_file_t* file = static_cast<lfs_file_t*>(priv);
        lfs_file_sync(fs_.get_lfs(), file);
        int res = lfs_file_close(fs_.get_lfs(), file);
        delete file;
        return res;
    }

    int get_size(void* priv) const override {
        if (!priv) return 0;
        lfs_file_t* file = static_cast<lfs_file_t*>(priv);
        return lfs_file_size(fs_.get_lfs(), file);
    }
};

#endif
