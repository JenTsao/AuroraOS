#include "ramfs.hpp"
#include "task.hpp"
#include <string.h> // for memcpy, memset
#ifndef AURORA_HOST_TEST
#include "autoconf.h"
#endif

#ifdef CONFIG_NO_DYNAMIC_ALLOCATION
static constexpr int kStaticSlotCount = 4;
static constexpr int kStaticSlotCapacity = 512;
static char ramfs_buffers[kStaticSlotCount][kStaticSlotCapacity];
static bool ramfs_buf_used[kStaticSlotCount] = {false, false, false, false};
#endif

// 单个文件最大允许大小：1GB（扩容硬上限，ES.45）
static constexpr long long kRamFileMaxCapacity = 1024LL * 1024LL * 1024LL;

// 依赖全局重载的 operator new
RamFile::RamFile(int capacity) : capacity_(0), file_size_(0) {
    data_ = nullptr;
#ifdef CONFIG_NO_DYNAMIC_ALLOCATION
    if (capacity <= 0 || capacity > kStaticSlotCapacity) {
        return;
    }
    {
        IrqGuard guard;
        for (int i = 0; i < kStaticSlotCount; i++) {
            if (!ramfs_buf_used[i]) {
                ramfs_buf_used[i] = true;
                data_ = ramfs_buffers[i];
                capacity_ = capacity;
                break;
            }
        }
    }
#else
    if (capacity <= 0 || capacity > kRamFileMaxCapacity) {
        return;
    }
    data_ = new char[capacity];
    if (data_) {
        capacity_ = capacity;
    }
#endif
    if (data_ && capacity_ > 0) {
        memset(data_, 0, static_cast<size_t>(capacity_));
    } else {
        capacity_ = 0;
    }
}

RamFile::~RamFile() {
#ifndef CONFIG_NO_DYNAMIC_ALLOCATION
    delete[] data_;
    data_ = nullptr;
#else
    if (data_) {
        IrqGuard guard;
        for (int i = 0; i < kStaticSlotCount; i++) {
            if (data_ == ramfs_buffers[i]) {
                ramfs_buf_used[i] = false;
                break;
            }
        }
        data_ = nullptr;
    }
#endif
    capacity_ = 0;
    file_size_ = 0;
}

int RamFile::read(char* buf, int len, int offset, void* /*priv*/) {
    if (!buf || offset < 0 || len < 0)
        return -1;
    if (len == 0 || !data_ || capacity_ <= 0 || file_size_ <= 0)
        return 0;

    int valid_size = (file_size_ <= capacity_) ? file_size_ : capacity_;
    if (offset >= valid_size)
        return 0; // EOF (End of File)

    int available = valid_size - offset;
    int bytes_to_read = (len < available) ? len : available;

    memcpy(buf, data_ + offset, static_cast<size_t>(bytes_to_read));
    return bytes_to_read;
}

int RamFile::write(const char* buf, int len, int offset, void* /*priv*/) {
    if (!buf || offset < 0 || len < 0)
        return -1;
    if (len == 0)
        return 0;

    // 若写入偏移已超出当前容量则返回 false；否则将 len 截断到容量内
    auto truncate_to_capacity = [&]() -> bool {
        if (!data_ || capacity_ <= 0 || offset >= capacity_)
            return false;
        int avail = capacity_ - offset;
        if (len > avail) {
            len = avail;
        }
        return len > 0;
    };

    int valid_size = (file_size_ < 0) ? 0 : ((file_size_ > capacity_) ? capacity_ : file_size_);
    long long required_capacity = static_cast<long long>(offset) + len;
#ifdef CONFIG_NO_DYNAMIC_ALLOCATION
    if (!data_ || capacity_ <= 0) {
        return -1;
    }
    // 无动态分配：写入超出现有容量或超出硬上限时，一律截断到容量内
    if (required_capacity < 0 || required_capacity > kRamFileMaxCapacity || required_capacity > capacity_) {
        if (!truncate_to_capacity())
            return 0;
    }
#else
    if (required_capacity < 0 || required_capacity > kRamFileMaxCapacity) { // 限制最大 1GB
        if (!truncate_to_capacity())
            return -1;
    } else if (!data_ || required_capacity > capacity_) {
        long long new_capacity = capacity_ <= 0 ? 512 : static_cast<long long>(capacity_) * 2;
        while (new_capacity < required_capacity) {
            long long next_cap = new_capacity * 2;
            if (next_cap <= new_capacity || next_cap > kRamFileMaxCapacity) {
                new_capacity = required_capacity;
                break;
            }
            new_capacity = next_cap;
        }
        if (new_capacity > kRamFileMaxCapacity) {
            new_capacity = kRamFileMaxCapacity;
        }

        char* new_data = new char[static_cast<size_t>(new_capacity)];
        if (!new_data) {
            // 扩容失败，截断写入
            if (!truncate_to_capacity())
                return -1; // 完全无法写入
        } else {
            // 扩容成功，仅拷贝有效数据 (valid_size) 并清零其余空间
            if (data_) {
                if (valid_size > 0) {
                    memcpy(new_data, data_, static_cast<size_t>(valid_size));
                }
                delete[] data_;
            }
            memset(new_data + valid_size, 0, static_cast<size_t>(new_capacity - valid_size));
            data_ = new_data;
            capacity_ = static_cast<int>(new_capacity);
        }
    }
#endif

    if (!data_ || capacity_ <= 0 || offset >= capacity_) {
        return 0;
    }

    // 稀疏写入：offset 超过当前 file_size_ 时，确保未扩容量路径也清零文件空洞 [valid_size, offset)
    if (offset > valid_size) {
        memset(data_ + valid_size, 0, static_cast<size_t>(offset - valid_size));
    }

    int avail = capacity_ - offset;
    int bytes_to_write = (len < avail) ? len : avail;
    if (bytes_to_write <= 0) {
        return 0;
    }
    memcpy(data_ + offset, buf, static_cast<size_t>(bytes_to_write));

    // 动态更新文件逻辑大小
    file_size_ = (offset + bytes_to_write > valid_size) ? (offset + bytes_to_write) : valid_size;
    return bytes_to_write;
}

int RamFile::truncate(int new_size) {
    if (new_size < 0)
        return -1;
    if (file_size_ > capacity_) {
        file_size_ = capacity_;
    }
    if (new_size < file_size_) {
        file_size_ = new_size;
        if (data_ && capacity_ > file_size_) {
            memset(data_ + file_size_, 0, static_cast<size_t>(capacity_ - file_size_));
        }
    }
    return 0;
}

void RamFile::clear() {
    file_size_ = 0;
    if (data_ && capacity_ > 0) {
        memset(data_, 0, static_cast<size_t>(capacity_));
    }
}

