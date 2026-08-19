// =============================================================================
// kernel/core/device.cpp
//
// 设备对象注册表 (DeviceRegistry) 核心实现
// =============================================================================
#include "device.hpp"
#include "../task/task.hpp"
#include <string.h>

DeviceRegistry& DeviceRegistry::instance() {
    static DeviceRegistry registry;
    return registry;
}

DeviceRegistry::DeviceRegistry() : device_count_(0) {
    for (size_t i = 0; i < MAX_DEVICES; ++i) {
        entries_[i].name[0] = '\0';
        entries_[i].device = nullptr;
        entries_[i].default_rights = 0;
        entries_[i].in_use = false;
    }
}

bool DeviceRegistry::register_device(Device* dev, uint32_t default_rights) {
    if (!dev || !dev->get_name())
        return false;

    LockGuard lock(registry_mutex_);

    const char* name = dev->get_name();
    size_t name_len = 0;
    while (name_len < DeviceEntry::NAME_MAX_LEN - 1 && name[name_len]) {
        name_len++;
    }

    if (name_len == 0)
        return false;

    // 1. 检查是否重名
    for (size_t i = 0; i < MAX_DEVICES; ++i) {
        if (entries_[i].in_use && strncmp(entries_[i].name, name, DeviceEntry::NAME_MAX_LEN) == 0) {
            return false; // 重复注册
        }
    }

    // 2. 查找空闲槽位
    int free_slot = -1;
    for (size_t i = 0; i < MAX_DEVICES; ++i) {
        if (!entries_[i].in_use) {
            free_slot = static_cast<int>(i);
            break;
        }
    }

    if (free_slot < 0)
        return false; // 注册表已满

    // 3. 填充设备表条目
    for (size_t j = 0; j < name_len; ++j) {
        entries_[free_slot].name[j] = name[j];
    }
    entries_[free_slot].name[name_len] = '\0';
    entries_[free_slot].device = dev;
    entries_[free_slot].default_rights = default_rights;
    entries_[free_slot].in_use = true;
    device_count_++;

    // 增加设备内核对象引用计数
    dev->retain();

    // 4. 将设备挂载到 VFS /dev/<name> 路径
    char* path = dev->vfs_path_storage();
    const int cap = dev->vfs_path_capacity();
    if (5 + static_cast<int>(name_len) + 1 <= cap) {
        path[0] = '/';
        path[1] = 'd';
        path[2] = 'e';
        path[3] = 'v';
        path[4] = '/';
        for (size_t j = 0; j < name_len; ++j) {
            path[5 + j] = name[j];
        }
        path[5 + name_len] = '\0';

        bool mounted = VfsManager::instance().mount(path, dev);
        dev->mark_registered(mounted);
    }

    return true;
}

bool DeviceRegistry::unregister_device(const char* name) {
    if (!name)
        return false;

    LockGuard lock(registry_mutex_);

    for (size_t i = 0; i < MAX_DEVICES; ++i) {
        if (entries_[i].in_use && strncmp(entries_[i].name, name, DeviceEntry::NAME_MAX_LEN) == 0) {
            Device* dev = entries_[i].device;

            if (dev) {
                dev->mark_registered(false);
                dev->release();
            }

            entries_[i].name[0] = '\0';
            entries_[i].device = nullptr;
            entries_[i].default_rights = 0;
            entries_[i].in_use = false;
            if (device_count_ > 0) {
                device_count_--;
            }
            return true;
        }
    }
    return false;
}

Device* DeviceRegistry::lookup_device(const char* name) const {
    if (!name)
        return nullptr;

    LockGuard lock(registry_mutex_);
    for (size_t i = 0; i < MAX_DEVICES; ++i) {
        if (entries_[i].in_use && strncmp(entries_[i].name, name, DeviceEntry::NAME_MAX_LEN) == 0) {
            return entries_[i].device;
        }
    }
    return nullptr;
}

size_t DeviceRegistry::get_device_count() const {
    LockGuard lock(registry_mutex_);
    return device_count_;
}

Device* DeviceRegistry::get_device(size_t index) const {
    LockGuard lock(registry_mutex_);
    size_t count = 0;
    for (size_t i = 0; i < MAX_DEVICES; ++i) {
        if (entries_[i].in_use) {
            if (count == index) {
                return entries_[i].device;
            }
            count++;
        }
    }
    return nullptr;
}

void DeviceRegistry::clear() {
    LockGuard lock(registry_mutex_);
    for (size_t i = 0; i < MAX_DEVICES; ++i) {
        if (entries_[i].in_use) {
            Device* dev = entries_[i].device;
            if (dev) {
                dev->mark_registered(false);
                dev->release();
            }
            entries_[i].name[0] = '\0';
            entries_[i].device = nullptr;
            entries_[i].default_rights = 0;
            entries_[i].in_use = false;
        }
    }
    device_count_ = 0;
}

int DeviceRegistry::open_device(TaskControlBlock* task, const char* name, uint32_t dst_slot,
                                uint32_t requested_rights) {
    if (!task || !name || !auroraos::kernel::CSpace::is_valid_slot(dst_slot)) {
        return -2; // 参数无效
    }

    LockGuard lock(registry_mutex_);

    // 1. 查找设备
    const DeviceEntry* entry = nullptr;
    for (size_t i = 0; i < MAX_DEVICES; ++i) {
        if (entries_[i].in_use && strncmp(entries_[i].name, name, DeviceEntry::NAME_MAX_LEN) == 0) {
            entry = &entries_[i];
            break;
        }
    }

    if (!entry || !entry->device) {
        return -1; // 设备未找到 (ENOENT)
    }

    // 2. 权限校验：请求权限不得超过设备注册时的 default_rights
    bool req_r = (requested_rights & auroraos::kernel::CAP_RIGHT_READ) != 0;
    bool req_w = (requested_rights & auroraos::kernel::CAP_RIGHT_WRITE) != 0;
    bool req_g = (requested_rights & auroraos::kernel::CAP_RIGHT_GRANT) != 0;

    bool allow_r = (entry->default_rights & auroraos::kernel::CAP_RIGHT_READ) != 0;
    bool allow_w = (entry->default_rights & auroraos::kernel::CAP_RIGHT_WRITE) != 0;
    bool allow_g = (entry->default_rights & auroraos::kernel::CAP_RIGHT_GRANT) != 0;

    if ((req_r && !allow_r) || (req_w && !allow_w) || (req_g && !allow_g)) {
        return -3; // 权限不足/禁止提权 (EACCES)
    }

    // 3. 打开设备
    Device* dev = entry->device;
    int open_res = dev->open();
    if (open_res != 0) {
        return -4; // 设备初始化/打开失败 (EIO)
    }

    // 4. 在目标槽位铸造设备能力 (先释放可能存在的旧能力)
    auroraos::kernel::CSpace::cap_delete(task, dst_slot);

    auroraos::kernel::Capability& cap = task->security.cspace[dst_slot];
    cap.type = auroraos::kernel::CapType::Device;
    cap.object = dev;
    dev->retain(); // 增加能力持有者引用

    cap.rights.read = req_r;
    cap.rights.write = req_w;
    cap.rights.grant = req_g;
    cap.badge = 0;

    return 0; // 成功
}

int DeviceRegistry::device_read(TaskControlBlock* task, uint32_t cap_slot, char* buf, int len, int offset) {
    if (!task || !buf || len <= 0)
        return -2;

    auroraos::kernel::Capability* cap = auroraos::kernel::CSpace::cap_lookup(task, cap_slot);
    if (!cap || cap->type != auroraos::kernel::CapType::Device || !cap->object) {
        return -1; // 无效能力槽位或非设备能力
    }

    if (!cap->rights.read) {
        return -3; // 无读权限
    }

    Device* dev = static_cast<Device*>(cap->object);
    return dev->read(buf, len, offset, nullptr);
}

int DeviceRegistry::device_write(TaskControlBlock* task, uint32_t cap_slot, const char* buf, int len, int offset) {
    if (!task || !buf || len <= 0)
        return -2;

    auroraos::kernel::Capability* cap = auroraos::kernel::CSpace::cap_lookup(task, cap_slot);
    if (!cap || cap->type != auroraos::kernel::CapType::Device || !cap->object) {
        return -1; // 无效能力槽位或非设备能力
    }

    if (!cap->rights.write) {
        return -3; // 无写权限
    }

    Device* dev = static_cast<Device*>(cap->object);
    return dev->write(buf, len, offset, nullptr);
}

int DeviceRegistry::device_ioctl(TaskControlBlock* task, uint32_t cap_slot, int request, void* arg) {
    if (!task)
        return -2;

    auroraos::kernel::Capability* cap = auroraos::kernel::CSpace::cap_lookup(task, cap_slot);
    if (!cap || cap->type != auroraos::kernel::CapType::Device || !cap->object) {
        return -1; // 无效能力槽位或非设备能力
    }

    Device* dev = static_cast<Device*>(cap->object);
    return dev->ioctl(request, arg, nullptr);
}

int DeviceRegistry::device_close(TaskControlBlock* task, uint32_t cap_slot) {
    if (!task || !auroraos::kernel::CSpace::is_valid_slot(cap_slot))
        return -2;

    auroraos::kernel::Capability* cap = auroraos::kernel::CSpace::cap_lookup(task, cap_slot);
    if (!cap || cap->type != auroraos::kernel::CapType::Device) {
        return -1;
    }

    Device* dev = static_cast<Device*>(cap->object);
    if (dev) {
        dev->close();
    }

    auroraos::kernel::CSpace::cap_delete(task, cap_slot);
    return 0;
}
