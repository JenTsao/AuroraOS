// =============================================================================
// test_power_management.cpp — 电源管理子系统增强测试
//
// 覆盖三层：
//   1. WakeLockRegistry        — 共享唤醒锁账本（引用计数/超时/类型/容量）
//   2. 内核 PowerManager        — 热保护、低电量降档、唤醒归因、WakeLock 门禁、
//                                 tickless 深睡决策、状态驻留功耗归因
//   3. 服务层 PowerManager      — 面向 App 的门面（委托账本 + 电池遥测 + Profile）
//
// 说明：ctest 通过 gtest_discover_tests 为每个 TEST 独立起进程，因此内核
// PowerManager / ChargingManager / WakeLockRegistry 这些单例在测试间天然隔离；
//  fixture 仍在 SetUp 中显式复位，保证直接运行整个二进制时也稳定。
// =============================================================================
#include <gtest/gtest.h>
#include "charging_manager.hpp"
#include "power/power_manager.hpp"
#include "power/wake_lock_registry.hpp"
#include "../../services/power/power_manager.hpp"
#include "../../metrics/power_profiler.hpp"

// -----------------------------------------------------------------------------
// 1. WakeLockRegistry
// -----------------------------------------------------------------------------
class WakeLockRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        WakeLockRegistry::instance().reset();
    }
    void TearDown() override {
        WakeLockRegistry::instance().reset();
    }
    WakeLockRegistry& reg() {
        return WakeLockRegistry::instance();
    }
};

TEST_F(WakeLockRegistryTest, AcquireReleaseRefCount) {
    EXPECT_EQ(reg().holder_count(), 0);
    EXPECT_EQ(reg().lock_count(), 0);

    EXPECT_TRUE(reg().acquire(100));
    EXPECT_TRUE(reg().is_held(100));
    EXPECT_FALSE(reg().is_held(200));
    EXPECT_EQ(reg().lock_count(), 1);
    EXPECT_EQ(reg().holder_count(), 1);

    // 同一 holder 再取一次 -> 引用计数叠加
    EXPECT_TRUE(reg().acquire(100));
    EXPECT_EQ(reg().lock_count(), 2);
    EXPECT_EQ(reg().holder_count(), 1);

    // 释放一次仍持有
    EXPECT_TRUE(reg().release(100));
    EXPECT_TRUE(reg().is_held(100));
    EXPECT_EQ(reg().lock_count(), 1);

    // 再释放 -> 槽位回收
    EXPECT_TRUE(reg().release(100));
    EXPECT_FALSE(reg().is_held(100));
    EXPECT_EQ(reg().holder_count(), 0);

    // 释放未登记的 holder 返回 false
    EXPECT_FALSE(reg().release(999));
}

TEST_F(WakeLockRegistryTest, SaturationAtUint16Max) {
    constexpr uint32_t kHolder = 0x1234;
    constexpr int kMax = 0xFFFF;
    for (int i = 0; i < kMax; ++i) {
        ASSERT_TRUE(reg().acquire(kHolder)) << "acquire #" << i;
    }
    EXPECT_EQ(reg().lock_count(), kMax);
    // 达到上限必须拒绝而不是回绕归零
    EXPECT_FALSE(reg().acquire(kHolder));
    EXPECT_TRUE(reg().is_held(kHolder));
    EXPECT_EQ(reg().lock_count(), kMax);

    reg().release_all(kHolder);
    EXPECT_FALSE(reg().is_held(kHolder));
    EXPECT_EQ(reg().lock_count(), 0);
}

TEST_F(WakeLockRegistryTest, TableFullRejectsNewHolder) {
    for (int i = 1; i <= WakeLockRegistry::MAX_HOLDERS; ++i) {
        ASSERT_TRUE(reg().acquire(static_cast<uint32_t>(i)));
    }
    EXPECT_EQ(reg().holder_count(), WakeLockRegistry::MAX_HOLDERS);
    // 表满：新 holder 无法登记，但已有 holder 仍可叠加引用
    EXPECT_FALSE(reg().acquire(static_cast<uint32_t>(WakeLockRegistry::MAX_HOLDERS + 1)));
    EXPECT_TRUE(reg().acquire(1));
    EXPECT_EQ(reg().lock_count(), WakeLockRegistry::MAX_HOLDERS + 1);
}

TEST_F(WakeLockRegistryTest, TimeoutExpiryReclaimsLock) {
    EXPECT_TRUE(reg().acquire(10, WakeLockType::PARTIAL, 1000));
    EXPECT_TRUE(reg().holds_any());
    EXPECT_EQ(reg().next_expiry_ms(), 1000u);

    // 未到期的 tick 只递减，不回收
    EXPECT_EQ(reg().on_tick(400), 0);
    EXPECT_TRUE(reg().holds_any());
    EXPECT_EQ(reg().next_expiry_ms(), 600u);

    // 越过剩余时间 -> 过期回收
    EXPECT_EQ(reg().on_tick(700), 1);
    EXPECT_FALSE(reg().holds_any());
    EXPECT_FALSE(reg().is_held(10));
    EXPECT_EQ(reg().next_expiry_ms(), WakeLockRegistry::NO_TIMEOUT);
}

TEST_F(WakeLockRegistryTest, NoTimeoutLockNeverExpires) {
    EXPECT_TRUE(reg().acquire(20, WakeLockType::PARTIAL, WakeLockRegistry::NO_TIMEOUT));
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(reg().on_tick(100000), 0);
    }
    EXPECT_TRUE(reg().holds_any());
    EXPECT_TRUE(reg().is_held(20));
}

TEST_F(WakeLockRegistryTest, ScreenLockUpgradesAndIsDetected) {
    EXPECT_TRUE(reg().acquire(30, WakeLockType::PARTIAL));
    EXPECT_TRUE(reg().holds_any());
    EXPECT_FALSE(reg().holds_screen());

    // 同一 holder 升级为 SCREEN
    EXPECT_TRUE(reg().acquire(30, WakeLockType::SCREEN));
    EXPECT_TRUE(reg().holds_screen());

    // 释放一次仍持 SCREEN（引用计数 2 -> 1）
    EXPECT_TRUE(reg().release(30));
    EXPECT_TRUE(reg().holds_screen());

    EXPECT_TRUE(reg().release(30));
    EXPECT_FALSE(reg().holds_screen());
    EXPECT_FALSE(reg().holds_any());
}

TEST_F(WakeLockRegistryTest, ReleaseAllClearsHolder) {
    reg().acquire(40);
    reg().acquire(40);
    reg().acquire(41);
    EXPECT_EQ(reg().lock_count(), 3);

    reg().release_all(40);
    EXPECT_FALSE(reg().is_held(40));
    EXPECT_TRUE(reg().is_held(41));
    EXPECT_EQ(reg().lock_count(), 1);
}

// -----------------------------------------------------------------------------
// 2. 内核 PowerManager 策略
// -----------------------------------------------------------------------------
class KernelPowerPolicyTest : public ::testing::Test {
protected:
    void SetUp() override {
        WakeLockRegistry::instance().reset();
        // 注入中性姿态（既不满足抬腕 z∈(800,1200)，也不满足落腕 |x/y|>750），
        // 确保任何用例都不会因上一个用例残留的 mock 数据而误触发抬腕唤醒。
        // 需要真实姿态的用例（如 WakeReasonWristRaise）会自行覆盖此基线。
        SensorManager::instance().get_accel_sensor().set_mock_data(0, 0, 0);
        MockBatteryDriver* mock = ChargingManager::instance().get_mock_driver();
        mock->set_voltage(4200);
        mock->set_temperature(25);
        mock->set_plugged(false);
        mock->set_state(ChargeState::DISCHARGING);

        PowerManager& pm = PowerManager::instance();
        // 健康电量下推一个 tick，清除任何残留的热/低电限流标志
        pm.on_tick(1000);
        pm.set_profile(PowerManager::Profile::BALANCED);
        pm.set_wake_deadline_provider(nullptr);
        pm.transition_to(PowerState::ACTIVE);
        Metrics::get_power_profiler().reset();
    }

    void TearDown() override {
        WakeLockRegistry::instance().reset();
        PowerManager::instance().set_wake_deadline_provider(nullptr);
        // 断掉加速度计供电，避免上一个用例注入的 mock 姿态数据（如 z=980）
        // 泄漏到后续用例，导致 IDLE tick 误触发抬腕唤醒。
        SensorManager::instance().get_accel_sensor().power_down();
        MockBatteryDriver* mock = ChargingManager::instance().get_mock_driver();
        mock->set_voltage(4200);
        mock->set_temperature(25);
        mock->set_plugged(false);
    }

    MockBatteryDriver* mock() {
        return ChargingManager::instance().get_mock_driver();
    }
};

TEST_F(KernelPowerPolicyTest, ThermalThrottleClampsActiveToDim) {
    PowerManager& pm = PowerManager::instance();
    mock()->set_temperature(55); // > 50°C -> OVERHEAT
    mock()->set_voltage(4000);   // 电量健康，排除低电干扰

    pm.on_tick(1000);

    EXPECT_TRUE(pm.is_thermal_throttled());
    // 过温时把已经亮着的屏幕压到 DIM，并强制最低功耗档位
    EXPECT_EQ(pm.get_state(), PowerState::DIM);
    EXPECT_EQ(pm.get_profile(), PowerManager::Profile::ULTRA_LOW_POWER);
    // 用户档位保留，恢复后据此回退
    EXPECT_EQ(pm.get_user_profile(), PowerManager::Profile::BALANCED);
}

TEST_F(KernelPowerPolicyTest, ThermalThrottleHysteresisRelease) {
    PowerManager& pm = PowerManager::instance();
    mock()->set_voltage(4000);

    mock()->set_temperature(55);
    pm.on_tick(1000);
    ASSERT_TRUE(pm.is_thermal_throttled());

    // 45°C：health 已 GOOD，但高于 40°C 释放阈值 -> 仍限流（滞回）
    mock()->set_temperature(45);
    pm.on_tick(1000);
    EXPECT_TRUE(pm.is_thermal_throttled());

    // 38°C：低于释放阈值 -> 解除限流并恢复用户档位
    mock()->set_temperature(38);
    pm.on_tick(1000);
    EXPECT_FALSE(pm.is_thermal_throttled());
    EXPECT_EQ(pm.get_profile(), PowerManager::Profile::BALANCED);
}

TEST_F(KernelPowerPolicyTest, LowBatteryDeratesProfile) {
    PowerManager& pm = PowerManager::instance();
    mock()->set_voltage(3450); // SOC ≈ 15% (< 20%)
    mock()->set_temperature(25);

    pm.on_tick(1000);

    EXPECT_TRUE(pm.is_low_battery_throttled());
    EXPECT_EQ(pm.get_profile(), PowerManager::Profile::POWER_SAVE);
    EXPECT_EQ(pm.get_user_profile(), PowerManager::Profile::BALANCED);
    EXPECT_EQ(pm.get_timeout_active_to_dim(), 3000u); // POWER_SAVE 超时
}

TEST_F(KernelPowerPolicyTest, LowBatteryHysteresisRelease) {
    PowerManager& pm = PowerManager::instance();

    mock()->set_voltage(3450); // 15% -> 触发降档
    pm.on_tick(1000);
    ASSERT_TRUE(pm.is_low_battery_throttled());

    mock()->set_voltage(3900); // ≈64% (>= 25%) -> 解除
    pm.on_tick(1000);
    EXPECT_FALSE(pm.is_low_battery_throttled());
    EXPECT_EQ(pm.get_profile(), PowerManager::Profile::BALANCED);
    EXPECT_EQ(pm.get_timeout_active_to_dim(), 5000u);
}

TEST_F(KernelPowerPolicyTest, CriticalEnterAndRecover) {
    PowerManager& pm = PowerManager::instance();

    mock()->set_voltage(3300); // SOC 0% -> critical low
    mock()->set_plugged(false);
    pm.on_tick(1000);
    EXPECT_EQ(pm.get_state(), PowerState::CRITICAL);

    // 电量恢复（未插电）-> 退出 CRITICAL 回到息屏待机
    mock()->set_voltage(4000);
    pm.on_tick(1000);
    EXPECT_EQ(pm.get_state(), PowerState::IDLE);
    EXPECT_EQ(pm.get_last_wake_reason(), WakeReason::BATTERY_RECOVER);
}

TEST_F(KernelPowerPolicyTest, WakeReasonChargerPlug) {
    PowerManager& pm = PowerManager::instance();
    pm.transition_to(PowerState::IDLE);
    mock()->set_voltage(4000);

    // 消费掉 SetUp 可能残留的插拔边沿
    ChargingManager::instance().has_just_plugged();
    ChargingManager::instance().has_just_unplugged();
    pm.on_tick(1000);
    EXPECT_EQ(pm.get_state(), PowerState::IDLE);

    mock()->set_plugged(true);
    pm.on_tick(1000);
    EXPECT_EQ(pm.get_state(), PowerState::ACTIVE);
    EXPECT_EQ(pm.get_last_wake_reason(), WakeReason::CHARGER_PLUG);
}

TEST_F(KernelPowerPolicyTest, WakeReasonWristRaise) {
    PowerManager& pm = PowerManager::instance();
    SensorManager::instance().get_accel_sensor().power_up();
    mock()->set_voltage(4000);
    pm.transition_to(PowerState::IDLE);

    // 稳定朝上姿态累计超过 1000ms -> 抬腕唤醒
    for (int i = 0; i < 40; ++i) {
        SensorManager::instance().get_accel_sensor().set_mock_data(0, 0, 980);
        pm.on_tick(30);
    }
    EXPECT_EQ(pm.get_state(), PowerState::ACTIVE);
    EXPECT_EQ(pm.get_last_wake_reason(), WakeReason::WRIST_RAISE);
}

TEST_F(KernelPowerPolicyTest, ScreenWakeLockPreventsDimming) {
    PowerManager& pm = PowerManager::instance();
    WakeLockRegistry::instance().acquire(1, WakeLockType::SCREEN, WakeLockRegistry::NO_TIMEOUT);
    pm.transition_to(PowerState::ACTIVE);

    // 超过 active->dim 超时，但 SCREEN 锁要求常亮
    pm.on_tick(6000);
    EXPECT_EQ(pm.get_state(), PowerState::ACTIVE);

    // 释放后下一个 tick 立即降档
    WakeLockRegistry::instance().release(1);
    pm.on_tick(1);
    EXPECT_EQ(pm.get_state(), PowerState::DIM);
}

TEST_F(KernelPowerPolicyTest, PartialWakeLockPreventsDeepSleepDownshift) {
    PowerManager& pm = PowerManager::instance();
    WakeLockRegistry::instance().acquire(2, WakeLockType::PARTIAL, WakeLockRegistry::NO_TIMEOUT);
    pm.transition_to(PowerState::IDLE);

    // 超过 idle->sleep 超时，但 PARTIAL 锁要求 CPU 可调度
    pm.on_tick(11000);
    EXPECT_EQ(pm.get_state(), PowerState::IDLE);

    WakeLockRegistry::instance().release(2);
    pm.on_tick(1);
    EXPECT_EQ(pm.get_state(), PowerState::SLEEP);
}

TEST_F(KernelPowerPolicyTest, DeepSleepCountAndWakeDeadlineVeto) {
    PowerManager& pm = PowerManager::instance();
    pm.transition_to(PowerState::SLEEP);
    const uint32_t base = pm.get_deep_sleep_count();

    // 无锁、无外部约束 -> 进入 tickless 深睡，计数递增
    pm.execute_wfi_if_needed();
    EXPECT_EQ(pm.get_deep_sleep_count(), base + 1);

    // 注册返回 0 的唤醒约束提供者 -> 强制退化为普通 WFI，不再深睡
    static uint32_t (*const veto)() = []() -> uint32_t { return 0; };
    pm.set_wake_deadline_provider(veto);
    pm.execute_wfi_if_needed();
    EXPECT_EQ(pm.get_deep_sleep_count(), base + 1);

    // PARTIAL 锁同样阻止 tickless 深睡
    pm.set_wake_deadline_provider(nullptr);
    WakeLockRegistry::instance().acquire(3, WakeLockType::PARTIAL, WakeLockRegistry::NO_TIMEOUT);
    pm.execute_wfi_if_needed();
    EXPECT_EQ(pm.get_deep_sleep_count(), base + 1);
    WakeLockRegistry::instance().release(3);
}

TEST_F(KernelPowerPolicyTest, NonSleepStateSkipsWfi) {
    PowerManager& pm = PowerManager::instance();
    pm.transition_to(PowerState::ACTIVE);
    const uint32_t base = pm.get_deep_sleep_count();
    // ACTIVE 态调用不应触发深睡路径
    EXPECT_EQ(pm.execute_wfi_if_needed(), 0u);
    EXPECT_EQ(pm.get_deep_sleep_count(), base);
}

TEST_F(KernelPowerPolicyTest, StateDurationAttribution) {
    PowerManager& pm = PowerManager::instance();
    Metrics::get_power_profiler().reset();
    pm.transition_to(PowerState::ACTIVE);

    pm.on_tick(1000); // 在 ACTIVE 停留 1000ms
    pm.transition_to(PowerState::DIM); // 切换时把 1000ms 归因到 ACTIVE

    // 仅 ACTIVE 1000ms -> 平均电流应等于 ACTIVE 档 15000uA
    EXPECT_EQ(Metrics::get_power_profiler().calculate_average_current_ua(), 15000u);
}

// -----------------------------------------------------------------------------
// 3. 服务层 PowerManager 门面
// -----------------------------------------------------------------------------
namespace svc = auroraos::power_service;

class PowerServiceFacadeTest : public ::testing::Test {
protected:
    void SetUp() override {
        svc::PowerManager::instance().reset();
    }
    void TearDown() override {
        svc::PowerManager::instance().reset();
    }
};

TEST_F(PowerServiceFacadeTest, DelegatesToSharedRegistry) {
    auto& pm = svc::PowerManager::instance();
    EXPECT_TRUE(pm.can_sleep());

    EXPECT_TRUE(pm.acquire_wake_lock(100));
    EXPECT_FALSE(pm.can_sleep());
    EXPECT_FALSE(pm.is_screen_held());
    // 内核侧账本与服务侧是同一份
    EXPECT_TRUE(WakeLockRegistry::instance().is_held(100));

    EXPECT_TRUE(pm.acquire_wake_lock(200, svc::WakeLockKind::SCREEN));
    EXPECT_TRUE(pm.is_screen_held());
    EXPECT_EQ(pm.get_wake_lock_count(), 2);

    pm.release_all_wake_locks(200);
    EXPECT_FALSE(pm.is_screen_held());
    EXPECT_TRUE(pm.release_wake_lock(100));
    EXPECT_TRUE(pm.can_sleep());
}

TEST_F(PowerServiceFacadeTest, OnTickExpiresLeakedLocks) {
    auto& pm = svc::PowerManager::instance();
    // 默认 30s 超时
    EXPECT_TRUE(pm.acquire_wake_lock(7));
    EXPECT_EQ(pm.get_expired_lock_count(), 0u);

    pm.on_tick(svc::PowerManager::DEFAULT_WAKE_LOCK_TIMEOUT_MS + 1);
    EXPECT_TRUE(pm.can_sleep());
    EXPECT_GE(pm.get_expired_lock_count(), 1u);
    EXPECT_EQ(pm.get_wake_lock_count(), 0);
}

TEST_F(PowerServiceFacadeTest, BatteryInfoRoundtrip) {
    auto& pm = svc::PowerManager::instance();
    pm.update_battery_info(85, 3900, 31, /*health*/ 0, /*charge*/ 2, /*plugged*/ true);

    svc::BatteryInfo info = pm.get_battery_info();
    EXPECT_EQ(info.level, 85);
    EXPECT_EQ(info.voltage_mv, 3900);
    EXPECT_EQ(info.temperature_c, 31);
    EXPECT_EQ(info.charge_state, 2);
    EXPECT_EQ(info.plugged, 1);

    // 越界电量被夹到 100
    pm.update_battery_level(150);
    EXPECT_EQ(pm.get_battery_level(), 100);
}

TEST_F(PowerServiceFacadeTest, ProfileSetGet) {
    auto& pm = svc::PowerManager::instance();
    pm.set_profile(svc::PowerProfile::ULTRA_SAVER);
    EXPECT_EQ(pm.get_profile(), svc::PowerProfile::ULTRA_SAVER);
    pm.set_profile(svc::PowerProfile::PERFORMANCE);
    EXPECT_EQ(pm.get_profile(), svc::PowerProfile::PERFORMANCE);
}
