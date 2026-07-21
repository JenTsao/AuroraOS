#include "ramfs.hpp"
#include <string.h> // for memcpy, memset
#ifndef AURORA_HOST_TEST
#include "autoconf.h"
#endif

#ifdef CONFIG_NO_DYNAMIC_ALLOCATION
static char ramfs_buffers[4][512];
static bool ramfs_buf_used[4] = {false, false, false, false};
#endif

// 依赖全局重载的 operator new
RamFile::RamFile(int capacity) : capacity_(capacity), file_size_(0) {
#ifdef CONFIG_NO_DYNAMIC_ALLOCATION
    if (capacity <= 1024) {
        data_ = nullptr;
        for (int i = 0; i < 4; i++) {
            if (!ramfs_buf_used[i]) {
                ramfs_buf_used[i] = true;
                data_ = ramfs_buffers[i];
                capacity_ = 1024;
                break;
            }
        }
        if (!data_) {
            capacity_ = 0;
        }
    } else {
        data_ = nullptr;
        capacity_ = 0;
    }
#else
    data_ = new char[capacity_];
#endif
    if (data_) {
        memset(data_, 0, capacity_);
    } else {
        capacity_ = 0;
    }
}

RamFile::~RamFile() {
#ifndef CONFIG_NO_DYNAMIC_ALLOCATION
    delete[] data_;
#else
    if (data_) {
        for (int i = 0; i < 4; i++) {
            if (data_ == ramfs_buffers[i]) {
                ramfs_buf_used[i] = false;
                break;
            }
        }
    }
#endif
}

int RamFile::read(char* buf, int len, int offset, void* priv) {
    if (!data_ || offset >= file_size_) return 0; // EOF (End of File)
    
    int bytes_to_read = len;
    if (len > file_size_ - offset) {
        bytes_to_read = file_size_ - offset;
    }

    memcpy(buf, data_ + offset, bytes_to_read);
    return bytes_to_read;
}

int RamFile::write(const char* buf, int len, int offset, void* priv) {
    // 检查溢出
    long long required_capacity = (long long)offset + len;
    if (required_capacity < 0 || required_capacity > 1073741824LL) { // 限制最大 1GB
        if (offset >= capacity_) return 0;
        len = capacity_ - offset;
    } else if (required_capacity > capacity_) {
#ifdef CONFIG_NO_DYNAMIC_ALLOCATION
        // 扩容失败，截断写入
        if (offset >= capacity_) return 0; // 完全无法写入
        len = capacity_ - offset;
#else
        long long new_capacity = capacity_ == 0 ? 512 : (long long)capacity_ * 2;
        while (new_capacity < required_capacity) {
            new_capacity *= 2;
        }
        
        char* new_data = new char[(size_t)new_capacity];
        if (!new_data) {
            // 扩容失败，截断写入
            if (offset >= capacity_) return 0; // 完全无法写入
            len = capacity_ - offset;
        } else {
            // 扩容成功，拷贝旧数据并初始化新空间
            if (data_) {
                memcpy(new_data, data_, capacity_);
                delete[] data_;
            }
            memset(new_data + capacity_, 0, new_capacity - capacity_);
            data_ = new_data;
            capacity_ = new_capacity;
        }
#endif
    }

    int bytes_to_write = len;
    if (data_) {
        memcpy(data_ + offset, buf, bytes_to_write);
    } else {
        return 0; // No memory available
    }

    // 动态更新文件逻辑大小
    if (offset + bytes_to_write > file_size_) {
        file_size_ = offset + bytes_to_write;
    }

    return bytes_to_write;
}
