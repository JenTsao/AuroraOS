// =============================================================================
// ui/vfs_font_reader.hpp
//
// AuroraOS VFS / LittleFS 字库读取适配器 (实现 IFontReader)
//
// 提供两种零堆适配方式：
//  1. VNodeFontReader — 直接挂载 VNode*（例如 LittleFsVNode / RamFile），零 IPC 开销
//  2. VfsFontReader   — 通过 VfsManager::instance() 跨进程按需 lseek + 分块 read
//                       （自动适配 VfsManager 单次 read <= 1024 B 的 IPC 负载上限）
// =============================================================================
#ifndef AURORA_UI_VFS_FONT_READER_HPP
#define AURORA_UI_VFS_FONT_READER_HPP

#include <stddef.h>
#include <stdint.h>
#include "../drivers/display/bitmap_font.hpp"
#include "../vfs/vfs.hpp"

namespace UI {

// 直连 VNode 的按需读取器（适用于内核服务内直接绑定 LittleFsVNode 或宿主单元测试）
class VNodeFontReader final : public IFontReader {
public:
    explicit constexpr VNodeFontReader(VNode* node = nullptr, void* priv = nullptr) noexcept
        : node_(node), priv_(priv), size_(0u) {}

    void attach(VNode* node, void* priv = nullptr) noexcept {
        node_ = node;
        priv_ = priv;
        refresh_size();
    }

    void refresh_size() noexcept {
        if (!node_) {
            size_ = 0u;
            return;
        }
        const int sz = node_->get_size(priv_);
        size_ = (sz > 0) ? static_cast<uint32_t>(sz) : 0u;
    }

    [[nodiscard]] uint32_t size() const noexcept override {
        if (size_ == 0u && node_) {
            const int sz = node_->get_size(priv_);
            return (sz > 0) ? static_cast<uint32_t>(sz) : 0u;
        }
        return size_;
    }

    [[nodiscard]] size_t read_at(uint32_t offset, uint8_t* out_buf, size_t len) noexcept override {
        const uint32_t total = size();
        if (!node_ || !out_buf || offset >= total || len == 0u || offset > 0x7FFFFFFFu) {
            return 0u;
        }
        const uint32_t avail = total - offset;
        const size_t target = (len < avail) ? len : static_cast<size_t>(avail);
        const int got =
            node_->read(reinterpret_cast<char*>(out_buf), static_cast<int>(target), static_cast<int>(offset), priv_);
        return (got > 0) ? static_cast<size_t>(got) : 0u;
    }

private:
    VNode* node_;
    void* priv_;
    uint32_t size_;
};

// 通过 VfsManager 客户端按需二分查找读取 .afont 文件（支持 LittleFS /lfs/... 等挂载点）
class VfsFontReader final : public IFontReader {
public:
    VfsFontReader() noexcept = default;

    ~VfsFontReader() noexcept override {
        close();
    }

    VfsFontReader(const VfsFontReader&) = delete;
    VfsFontReader& operator=(const VfsFontReader&) = delete;

    [[nodiscard]] bool open(const char* path) noexcept {
        close();
        if (!path) {
            return false;
        }
#if defined(CONFIG_VFS) || defined(AURORA_HOST_TEST)
        auto& vfs = VfsManager::instance();
        const int fd = vfs.open(path, 0);
        if (fd < 0) {
            return false;
        }
        const int sz = vfs.lseek(fd, 0, 2); // SEEK_END
        if (sz <= 0) {
            vfs.close(fd);
            return false;
        }
        fd_ = fd;
        size_ = static_cast<uint32_t>(sz);
        return true;
#else
        return false;
#endif
    }

    void close() noexcept {
#if defined(CONFIG_VFS) || defined(AURORA_HOST_TEST)
        if (fd_ >= 0) {
            VfsManager::instance().close(fd_);
        }
#endif
        fd_ = -1;
        size_ = 0u;
    }

    [[nodiscard]] bool is_open() const noexcept {
        return fd_ >= 0;
    }

    [[nodiscard]] uint32_t size() const noexcept override {
        return size_;
    }

    [[nodiscard]] size_t read_at(uint32_t offset, uint8_t* out_buf, size_t len) noexcept override {
#if defined(CONFIG_VFS) || defined(AURORA_HOST_TEST)
        if (fd_ < 0 || !out_buf || offset >= size_ || len == 0u || offset > 0x7FFFFFFFu) {
            return 0u;
        }
        auto& vfs = VfsManager::instance();
        if (vfs.lseek(fd_, static_cast<int>(offset), 0) < 0) {
            return 0u;
        }
        const uint32_t avail = size_ - offset;
        const size_t target = (len < avail) ? len : static_cast<size_t>(avail);
        size_t done = 0u;
        while (done < target) {
            const size_t rem = target - done;
            // VfsManager::read 单次 IPC 负载上限为 1024 B，这里分块至 512 B 安全读取
            const int chunk = static_cast<int>(rem > 512u ? 512u : rem);
            const int got = vfs.read(fd_, reinterpret_cast<char*>(out_buf + done), chunk);
            if (got <= 0) {
                break;
            }
            done += static_cast<size_t>(got);
        }
        return done;
#else
        (void)offset;
        (void)out_buf;
        (void)len;
        return 0u;
#endif
    }

private:
    int fd_{-1};
    uint32_t size_{0u};
};

} // namespace UI

#endif // AURORA_UI_VFS_FONT_READER_HPP
