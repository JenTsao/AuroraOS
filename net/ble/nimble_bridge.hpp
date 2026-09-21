#ifndef AURORA_NIMBLE_BRIDGE_HPP
#define AURORA_NIMBLE_BRIDGE_HPP

#include <stdint.h>
#include <stddef.h>
#include "hal_ble.hpp"
#include "ble_stealth.hpp"

namespace auroraos {
namespace ble {

// ============================================================================
// NimbleBridge — NimBLE Host 栈与 AuroraOS 硬件/安全子系统的桥接协调器
//
// 负责：
//   1. 协调 NimBLE Host 栈生命周期 (nimble_port_init / step / run)
//   2. 桥接底层 HciTransport (UART H4 / 内存 IPC) 与 Host 数据通道
//   3. 联动安全模块：在 Host 启动/同步/断开时，注入 BleStealth 隐身伪装
//      并驱动 BleIds / BleMitmDetector / GattAuditor 安全检测
// ============================================================================
class NimbleBridge {
private:
    bool initialized_{false};
    bool synced_{false};
    bool advertising_{false};

    NimbleBridge() = default;

public:
    static NimbleBridge& instance() noexcept {
        static NimbleBridge bridge;
        return bridge;
    }

    // 初始化 NimBLE Host 栈并绑定 HCI 传输层
    bool init() noexcept;

    // 驱动 NimBLE Host 事件队列单步执行 (可供后台守护任务调度)
    void step(uint32_t timeout_ms = 0) noexcept;

    // 检查 Host 与 Controller 链路是否完成时钟与参数同步
    bool is_synced() const noexcept { return synced_; }

    // 检查是否正在广播
    bool is_advertising() const noexcept { return advertising_; }

    // 启动广播 (根据系统配置自动选择标准广播或 BleStealth 隐身伪装广播)
    bool start_advertising() noexcept;

    // 停止广播
    bool stop_advertising() noexcept;

    // 主机同步/复位内部回调
    void on_host_sync() noexcept;
    void on_host_reset(int reason) noexcept;
};

} // namespace ble
} // namespace auroraos

#endif // AURORA_NIMBLE_BRIDGE_HPP
