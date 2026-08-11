#ifndef AURORA_DEVICE_HPP
#define AURORA_DEVICE_HPP

#include "vfs.hpp" // 引入我们之前的 VFS 节点定义
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include "mutex.hpp"

// 设备类型枚举
enum class DeviceType {
    Unknown,
    Char,   // 字符设备（如 UART, SPI, 传感器）
    Block   // 块设备（如 Flash, SD 卡）
};

// 继承自 VNode 的统一 Device 基类
class Device : public VNode {
protected:
    const char* name_;
    DeviceType type_;
    // VFS 路径固定内联存储，避免运行期动态分配（配合 CONFIG_NO_DYNAMIC_ALLOCATION）。
    // 容量与 VfsManager 的 MountPoint::path (char[32]) 对齐。
    static constexpr int VFS_PATH_CAP = 32;
    char vfs_path_buf_[VFS_PATH_CAP] = {0};
    bool registered_ = false;

public:
    Device(const char* name, DeviceType type) : name_(name), type_(type) {}
    virtual ~Device() = default;

    const char* get_vfs_path() const { return registered_ ? vfs_path_buf_ : nullptr; }
    // 返回内联缓冲区首地址供注册流程原地拼装路径；标记为已注册。
    char* vfs_path_storage() { return vfs_path_buf_; }
    int vfs_path_capacity() const { return VFS_PATH_CAP; }
    void mark_registered(bool v) { registered_ = v; }

    DeviceType get_type() const { return type_; }
    const char* get_name() const { return name_; }

    // 设备的生命周期与控制接口
    virtual int open() { return 0; }
    virtual int close() { return 0; }
    virtual int ioctl(int /*request*/, void* /*arg*/, void* /*priv*/) override { return -1; }
};

// 字符设备派生类
class CharDevice : public Device {
public:
    CharDevice(const char* name) : Device(name, DeviceType::Char) {}
    
    // 字符设备按字节流读写
    virtual int read(char* /*buf*/, int /*len*/, int /*offset*/, void* /*priv*/) override { return -1; }
    virtual int write(const char* /*buf*/, int /*len*/, int /*offset*/, void* /*priv*/) override { return -1; }
};

// 块设备派生类
class BlockDevice : public Device {
public:
    BlockDevice(const char* name) : Device(name, DeviceType::Block) {}
    
    // 块设备通常需要扇区对齐的读写
    virtual int read_blocks(uint32_t /*block_addr*/, uint32_t /*offset*/, uint8_t* /*buf*/, uint32_t /*size*/) { return -1; }
    virtual int write_blocks(uint32_t /*block_addr*/, uint32_t /*offset*/, const uint8_t* /*buf*/, uint32_t /*size*/) { return -1; }
};

// 设备注册表：负责将设备对象绑定到 VFS
class DeviceRegistry {
private:
    Mutex registry_mutex_;
public:
    static DeviceRegistry& instance() {
        static DeviceRegistry registry;
        return registry;
    }

    // 将设备注册到 /dev/ 路径下
    bool register_device(Device* dev) {
        LockGuard lock(registry_mutex_);
        if (dev->get_vfs_path() != nullptr) return false;

        const char* name = dev->get_name();
        int name_len = 0;
        while (name[name_len]) name_len++;

        // 目标格式 "/dev/<name>" + '\0'，需要 5 + name_len + 1 字节。
        // 直接写入设备内联缓冲，无动态分配（兼容 CONFIG_NO_DYNAMIC_ALLOCATION）。
        char* path = dev->vfs_path_storage();
        const int cap = dev->vfs_path_capacity();
        if (5 + name_len + 1 > cap) return false; // 名字过长，放不下

        path[0] = '/'; path[1] = 'd'; path[2] = 'e'; path[3] = 'v'; path[4] = '/';
        for (int j = 0; j < name_len; j++) {
            path[5 + j] = name[j];
        }
        path[5 + name_len] = '\0';

        // mount 内部会 str_copy 走路径内容，不长期持有该指针，因此内联缓冲即可。
        bool res = VfsManager::instance().mount(path, dev);
        dev->mark_registered(res);
        return res;
    }
};

#endif
