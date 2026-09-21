#include "nimble_bridge.hpp"
#include "hci/hci_transport.hpp"
#include "ble_ids.hpp"
#include "ble_mitm.hpp"
#include "gatt_auditor.hpp"
#include <string.h>

// 弱引用声明 NimBLE C 接口，在固件构建中由 nimble_host 静态库提供，
// 在单元测试或无 NimBLE 目标的构建中自动退化为 nullptr 安全回退。
extern "C" {
__attribute__((weak)) void nimble_port_init(void);
__attribute__((weak)) void nimble_port_step(uint32_t timeout_ms);
}

namespace auroraos {
namespace ble {

bool NimbleBridge::init() noexcept {
    if (initialized_)
        return true;

    // 1. 初始化底层 HCI 传输层 (若尚未初始化)
    if (hci::g_hci_transport) {
        hci::g_hci_transport->init();
    }

    // 2. 初始化 NimBLE Host 栈 (若符号可用)
    if (nimble_port_init) {
        nimble_port_init();
    }

    initialized_ = true;
    synced_ = true; // 传输层就绪

    // 3. 注册 GATT 审计与基础 IDS 服务
    BleIds::instance().load_default_rules();

    // 4. 自动激活广播
    start_advertising();

    return true;
}

void NimbleBridge::step(uint32_t timeout_ms) noexcept {
    if (!initialized_)
        return;

    if (nimble_port_step) {
        nimble_port_step(timeout_ms);
    }
}

bool NimbleBridge::start_advertising() noexcept {
    if (!initialized_)
        return false;

    auto preset = ble_stealth_preset_from_config();
    if (preset == BleStealthPreset::NONE) {
        // 正常模式：标准 Discoverable 广播
        HalBle::start_advertising("Aurora_MiBand8");
    } else {
        // 隐身模式：注入 Apple iBeacon / AirPods 等伪装指纹并隐藏 GAP Discoverable
        uint8_t ad_data[BLE_ADV_MAX_LEN];
        BleStealth::instance().set_preset(preset);
        size_t len = BleStealth::instance().build_advertisement(ad_data, sizeof(ad_data));
        HalBle::start_advertising_raw(ad_data, len);
    }

    advertising_ = true;
    return true;
}

bool NimbleBridge::stop_advertising() noexcept {
    HalBle::stop_advertising();
    advertising_ = false;
    return true;
}

void NimbleBridge::on_host_sync() noexcept {
    synced_ = true;
    start_advertising();
}

void NimbleBridge::on_host_reset(int reason) noexcept {
    (void)reason;
    synced_ = false;
    advertising_ = false;
}

} // namespace ble
} // namespace auroraos
