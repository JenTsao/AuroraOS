// =============================================================================
// security/hids/file_integrity.hpp
//
// HIDS 文件完整性监控：基于 FNV-1a 哈希的基线校验
//
//   - fnv1a32()：零依赖、确定性的 32 位哈希
//   - FileIntegrityMonitor：注册受监控文件 → 建立基线 → 校验并报告篡改
//   - baseline()/verify() 经 VFS 读取文件（固件）；verify_content() 支持内存内容校验（测试）
//
// 设计原则（遵循 AGENTS.md）：固定数组、零堆分配、noexcept
// =============================================================================
#ifndef AURORA_HIDS_FILE_INTEGRITY_HPP
#define AURORA_HIDS_FILE_INTEGRITY_HPP

#include <stdint.h>
#include "../../vfs/vfs.hpp"

namespace aurora {
namespace hids {

class FileIntegrityMonitor {
public:
    static constexpr int kMaxEntries = 16;
    static constexpr int kMaxPathLen = 48;
    static constexpr int kReadBufSize = 256;

    // FNV-1a 32 位哈希（确定性、零依赖）
    static uint32_t fnv1a32(const uint8_t* data, int len) noexcept {
        uint32_t hash = 2166136261u;
        for (int i = 0; i < len; ++i) {
            hash ^= data[i];
            hash *= 16777619u;
        }
        return hash;
    }

    static uint32_t fnv1a32_str(const char* s) noexcept {
        uint32_t hash = 2166136261u;
        while (*s) {
            hash ^= static_cast<uint8_t>(*s++);
            hash *= 16777619u;
        }
        return hash;
    }

    // 注册受监控文件路径（幂等）
    bool add_path(const char* path) noexcept {
        if (!path)
            return false;
        for (int i = 0; i < kMaxEntries; ++i) {
            if (entries_[i].active && str_eq_(entries_[i].path, path))
                return true;
        }
        for (int i = 0; i < kMaxEntries; ++i) {
            if (!entries_[i].active) {
                copy_str_(entries_[i].path, path, kMaxPathLen);
                entries_[i].baseline_hash = 0;
                entries_[i].active = true;
                return true;
            }
        }
        return false;
    }

    // 直接设置基线哈希（测试/内存场景）
    bool set_baseline(const char* path, uint32_t hash) noexcept {
        IntegrityEntry* e = find_(path);
        if (!e) {
            if (!add_path(path))
                return false;
            e = find_(path);
        }
        if (!e)
            return false;
        e->baseline_hash = hash;
        return true;
    }

    // 从 VFS 读取所有已注册文件并建立基线
    void baseline() {
        char buf[kReadBufSize];
        for (int i = 0; i < kMaxEntries; ++i) {
            if (!entries_[i].active)
                continue;
            const int n = read_file_(entries_[i].path, buf, kReadBufSize);
            entries_[i].baseline_hash = (n > 0) ? fnv1a32(reinterpret_cast<uint8_t*>(buf), n) : 0;
        }
    }

    // 校验所有已注册文件（VFS），返回被篡改文件数
    int verify() {
        char buf[kReadBufSize];
        int findings = 0;
        for (int i = 0; i < kMaxEntries; ++i) {
            if (!entries_[i].active)
                continue;
            const int n = read_file_(entries_[i].path, buf, kReadBufSize);
            const uint32_t h = (n > 0) ? fnv1a32(reinterpret_cast<uint8_t*>(buf), n) : 0;
            if (entries_[i].baseline_hash != 0 && h != entries_[i].baseline_hash) {
                ++findings;
                record_(entries_[i].path, "file modified");
            }
        }
        return findings;
    }

    // 内存内容校验：内容哈希与基线不符则判定被篡改
    bool verify_content(const char* path, const uint8_t* data, int len) {
        IntegrityEntry* e = find_(path);
        if (!e || e->baseline_hash == 0)
            return false;
        if (fnv1a32(data, len) != e->baseline_hash) {
            record_(path, "content modified");
            return true;
        }
        return false;
    }

    // 统一检测入口（供 HidsEngine 调用）
    int scan() {
        return verify();
    }

    const char* get_name() const noexcept {
        return "file_integrity";
    }

    uint32_t get_total_findings() const noexcept {
        return total_findings_;
    }

    const char* get_last_finding() const noexcept {
        return last_finding_;
    }

    void reset() noexcept {
        for (int i = 0; i < kMaxEntries; ++i)
            entries_[i] = IntegrityEntry{};
        total_findings_ = 0;
        last_finding_[0] = '\0';
    }

private:
    struct IntegrityEntry {
        char path[kMaxPathLen];
        uint32_t baseline_hash;
        bool active;
    };

    IntegrityEntry entries_[kMaxEntries]{};
    uint32_t total_findings_ = 0;
    char last_finding_[64]{};

    IntegrityEntry* find_(const char* path) noexcept {
        for (int i = 0; i < kMaxEntries; ++i)
            if (entries_[i].active && str_eq_(entries_[i].path, path))
                return &entries_[i];
        return nullptr;
    }

    int read_file_(const char* path, char* buf, int len) {
        const int fd = VfsManager::instance().open(path);
        if (fd < 0)
            return -1;
        const int n = VfsManager::instance().read(fd, buf, len);
        VfsManager::instance().close(fd);
        return n;
    }

    void record_(const char* path, const char* what) {
        ++total_findings_;
        char* p = last_finding_;
        const char* const end = last_finding_ + sizeof(last_finding_) - 1;
        while (*path && p < end)
            *p++ = *path++;
        if (p < end)
            *p++ = ':';
        if (p < end)
            *p++ = ' ';
        while (*what && p < end)
            *p++ = *what++;
        *p = '\0';
    }

    static bool str_eq_(const char* a, const char* b) noexcept {
        while (*a && *b) {
            if (*a != *b)
                return false;
            ++a;
            ++b;
        }
        return *a == *b;
    }

    static void copy_str_(char* dst, const char* src, int maxlen) noexcept {
        int i = 0;
        while (src[i] && i < maxlen - 1) {
            dst[i] = src[i];
            ++i;
        }
        dst[i] = '\0';
    }
};

} // namespace hids
} // namespace aurora

#endif // AURORA_HIDS_FILE_INTEGRITY_HPP
