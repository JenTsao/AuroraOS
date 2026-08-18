// =============================================================================
// kernel/core/device.hpp
//
// 统一设备抽象模型与设备能力注册表 (DeviceRegistry)
// 核心思想："设备即对象" (Device-as-an-Object) 能力模型
//
// 架构设计：
//   1. Device 继承自 KernelObject (ObjectType::Device) 与 VNode：
//      - 对内：作为微内核能力对象，受 CSpace / Capability 权限管控
//      - 对外：挂载到 VFS /dev/<name> 命名空间，保持与 POSIX 兼容
//   2. DeviceRegistry：
//      - 全局设备对象表，维护设备名 -> Device* 映射及默认权限掩码
//      - 提供 sys_open_device / sys_device_read / sys_device_write 等能力调用支撑
//      - 严格禁止未授权或提权访问（Privilege Escalation Protection）
// =============================================================================
#ifndef AURORA_DEVICE_HPP
#define AURORA_DEVICE_HPP

#include <stdint.h>
#include <stddef.h>
#include "kernel_object.hpp"
#include "cspace.hpp"
#include "vfs.hpp"
#include "mutex.hpp"

struct TaskControlBlock;

// 设备类型枚举
enum class DeviceType {
    Unknown,
    Char, // 字符设备（如 UART, 触控, 传感器）
    Block // 块设备（如 Flash, SD 卡）
};

// 继承自 KernelObject 与 VNode 的统一设备对象基类
class Device : public auroraos::kernel::KernelObject, public VNode {
protected:
    const char* name_;
    DeviceType type_;
    static constexpr int VFS_PATH_CAP = 32;
    char vfs_path_buf_[VFS_PATH_CAP] = {0};
    bool registered_ = false;

public:
    Device(const char* name, DeviceType type)
        : auroraos::kernel::KernelObject(auroraos::kernel::ObjectType::Device),
          name_(name),
          type_(type) {}

    virtual ~Device() override = default;

    const char* get_vfs_path() const {
        return registered_ ? vfs_path_buf_ : nullptr;
    }

    char* vfs_path_storage() {
        return vfs_path_buf_;
    }

    int vfs_path_capacity() const {
        return VFS_PATH_CAP;
    }

    void mark_registered(bool v) {
        registered_ = v;
    }

    DeviceType get_device_type() const noexcept {
        return type_;
    }

    const char* get_name() const {
        return name_;
    }

    // 设备的生命周期与控制接口
    virtual int open() {
        return 0;
    }

    virtual int close() {
        return 0;
    }

    virtual int ioctl(int /*request*/, void* /*arg*/, void* /*priv*/) override {
        return -1;
    }

    virtual int read(char* /*buf*/, int /*len*/, int /*offset*/, void* /*priv*/) override {
        return -1;
    }

    virtual int write(const char* /*buf*/, int /*len*/, int /*offset*/, void* /*priv*/) override {
        return -1;
    }
};

// 字符设备派生类
class CharDevice : public Device {
public:
    explicit CharDevice(const char* name) : Device(name, DeviceType::Char) {}
};

// 块设备派生类
class BlockDevice : public Device {
public:
    explicit BlockDevice(const char* name) : Device(name, DeviceType::Block) {}

    virtual int read_blocks(uint32_t /*block_addr*/, uint32_t /*offset*/, uint8_t* /*buf*/, uint32_t /*size*/) {
        return -1;
    }

    virtual int write_blocks(uint32_t /*block_addr*/, uint32_t /*offset*/, const uint8_t* /*buf*/, uint32_t /*size*/) {
        return -1;
    }
};

// 设备表单项条目
struct DeviceEntry {
    static constexpr size_t NAME_MAX_LEN = 32;
    char name[NAME_MAX_LEN];
    Device* device;
    uint32_t default_rights;
    bool in_use;
};

// 设备对象注册表：负责将设备对象管理、能力铸造与 VFS 挂载
class DeviceRegistry {
public:
    static constexpr size_t MAX_DEVICES = 16;

    static DeviceRegistry& instance();

    // 注册设备对象并挂载到 VFS /dev/<name>
    bool register_device(Device* dev,
                         uint32_t default_rights = auroraos::kernel::CAP_RIGHT_READ |
                                                   auroraos::kernel::CAP_RIGHT_WRITE |
                                                   auroraos::kernel::CAP_RIGHT_GRANT);

    // 注销设备
    bool unregister_device(const char* name);

    // 名字查找
    Device* lookup_device(const char* name) const;

    // 遍历访问
    size_t get_device_count() const;
    Device* get_device(size_t index) const;

    // 清理（用于测试）
    void clear();

    // ========================================================
    // 能力模型核心接口 (供 SyscallDispatcher 调用)
    // ========================================================

    // 为目标任务打开设备并铸造能力到 dst_slot
    int open_device(TaskControlBlock* task, const char* name, uint32_t dst_slot, uint32_t requested_rights);

    // 基于能力的设备读取操作 (校验 CapType::Device 与 read 权限)
    int device_read(TaskControlBlock* task, uint32_t cap_slot, char* buf, int len, int offset);

    // 基于能力的设备写入操作 (校验 CapType::Device 与 write 权限)
    int device_write(TaskControlBlock* task, uint32_t cap_slot, const char* buf, int len, int offset);

    // 基于能力的设备控制操作
    int device_ioctl(TaskControlBlock* task, uint32_t cap_slot, int request, void* arg);

    // 关闭设备能力
    int device_close(TaskControlBlock* task, uint32_t cap_slot);

private:
    DeviceRegistry();
    ~DeviceRegistry() = default;

    DeviceRegistry(const DeviceRegistry&) = delete;
    DeviceRegistry& operator=(const DeviceRegistry&) = delete;

    mutable Mutex registry_mutex_;
    DeviceEntry entries_[MAX_DEVICES];
    size_t device_count_;
};

#endif // AURORA_DEVICE_HPP
