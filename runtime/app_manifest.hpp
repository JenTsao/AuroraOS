#ifndef AURORAOS_RUNTIME_APP_MANIFEST_HPP
#define AURORAOS_RUNTIME_APP_MANIFEST_HPP

#include <stdint.h>

namespace auroraos {
namespace runtime {

// 应用请求的系统能力枚举（位掩码）
enum class AppCapability : uint32_t {
    None         = 0,
    Network      = 1 << 0, // 允许访问网络服务
    FileSystem   = 1 << 1, // 允许访问 VFS (持久化存储)
    UI           = 1 << 2, // 允许创建窗口和绘制 UI
    Sensor       = 1 << 3, // 允许读取传感器数据
    Hardware     = 1 << 4, // 允许直接访问 HAL 或底层硬件 (危险)
    PowerControl = 1 << 5  // 允许请求阻止系统休眠
};

inline AppCapability operator|(AppCapability a, AppCapability b) {
    return static_cast<AppCapability>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline AppCapability operator&(AppCapability a, AppCapability b) {
    return static_cast<AppCapability>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

// 统一的 App Manifest 结构，定义了 App 的身份与资源边界
struct AppManifest {
    char name[32];               // 应用名称
    char version[16];            // 应用版本
    char author[32];             // 开发者
    
    uint32_t required_caps;      // 需要的权限 (AppCapability 的位掩码组合)
    
    uint32_t max_memory_bytes;   // 最大可用堆内存限制 (如 64KB)
    uint32_t max_cpu_percent;    // 最大 CPU 占用率 (0~100)
    uint32_t priority;           // 默认调度优先级映射
};

} // namespace runtime
} // namespace auroraos

#endif // AURORAOS_RUNTIME_APP_MANIFEST_HPP
