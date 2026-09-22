#ifndef AURORAOS_RUNTIME_APP_MANIFEST_HPP
#define AURORAOS_RUNTIME_APP_MANIFEST_HPP

#include <stdint.h>

namespace auroraos {
namespace runtime {

// 应用请求的系统能力枚举（位掩码）
enum class AppCapability : uint32_t {
    None = 0,
    Network = 1 << 0,      // 允许访问网络服务
    FileSystem = 1 << 1,   // 允许访问 VFS (持久化存储)
    UI = 1 << 2,           // 允许创建窗口和绘制 UI
    Sensor = 1 << 3,       // 允许读取传感器数据
    Hardware = 1 << 4,     // 允许直接访问 HAL 或底层硬件 (危险)
    PowerControl = 1 << 5, // 允许请求阻止系统休眠
    Timer = 1 << 6,        // 允许创建与管理定时器
    IPC = 1 << 7           // 允许跨进程 IPC 通信
};

inline AppCapability operator|(AppCapability a, AppCapability b) {
    return static_cast<AppCapability>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline AppCapability operator&(AppCapability a, AppCapability b) {
    return static_cast<AppCapability>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline AppCapability operator~(AppCapability a) {
    return static_cast<AppCapability>(~static_cast<uint32_t>(a));
}

constexpr uint32_t AURORA_MANIFEST_MAGIC = 0x4155524D; // 'AURM'
constexpr uint16_t AURORA_MANIFEST_VERSION = 1;

// 统一的 App Manifest 结构，定义了 App 的身份与资源边界
struct alignas(8) AppManifest {
    uint32_t magic = AURORA_MANIFEST_MAGIC;
    uint16_t manifest_version = AURORA_MANIFEST_VERSION;
    uint16_t flags = 0;

    char name[32] = {0};    // 应用名称
    char version[16] = {0}; // 应用版本
    char author[32] = {0};  // 开发者

    uint32_t required_caps = 0; // 需要的权限 (AppCapability 的位掩码组合)

    uint32_t max_memory_bytes = 0; // 最大可用堆内存限制 (如 64KB, 0 表示无限制)
    uint32_t max_cpu_percent = 0;  // 最大 CPU 占用率 (0~100)
    uint32_t priority = 8;         // 默认调度优先级映射 (TaskPriority::Low = 8)
    uint32_t stack_size_pow2 = 12; // 栈对齐指数 (2^12 = 4096 字节)

    bool is_valid() const noexcept {
        if (magic != AURORA_MANIFEST_MAGIC) return false;
        if (manifest_version == 0 || manifest_version > AURORA_MANIFEST_VERSION) return false;
        if (name[0] == '\0') return false;
        if (max_cpu_percent > 100) return false;
        return true;
    }

    bool has_capability(AppCapability cap) const noexcept {
        return (required_caps & static_cast<uint32_t>(cap)) != 0;
    }
};

} // namespace runtime
} // namespace auroraos

#endif // AURORAOS_RUNTIME_APP_MANIFEST_HPP
