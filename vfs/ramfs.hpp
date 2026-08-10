#ifndef RAMFS_HPP
#define RAMFS_HPP

#include "vfs.hpp"

// 继承自 VNode 的内存常规文件
class RamFile : public VNode {
private:
    char* data_;
    int capacity_;
    int file_size_;

public:
    // 动态扩容的内存常规文件
    // 初始化时在堆上开辟指定容量的内存
    explicit RamFile(int capacity = 512);
    ~RamFile();

    // Rule of Five compliance (C.21)
    RamFile(const RamFile&) = delete;
    RamFile& operator=(const RamFile&) = delete;

    int read(char* buf, int len, int offset, void* priv) override;
    int write(const char* buf, int len, int offset, void* priv) override;
    
    int get_size(void* priv) const override { return file_size_; }
};

#endif
