#include <gtest/gtest.h>
#include "charging_manager.hpp"
#include "power/power_manager.hpp"

class ChargingManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 每次测试前，将电池状态恢复为满电、未插拔的健康状态
        MockBatteryDriver* mock = ChargingManager::instance().get_mock_driver();
        mock->set_voltage(4200);
        mock->set_plugged(false);
        mock->set_state(ChargeState::DISCHARGING);
        
        // 推一个 tick 强制更新状态
        ChargingManager::instance().on_tick(1000); 
    }
};

TEST_F(ChargingManagerTest, CalculateSocBoundaryValues) {
    MockBatteryDriver* mock = ChargingManager::instance().get_mock_driver();
    
    // 4.2V 以上，电量必须是 100%
    mock->set_voltage(4200);
    ChargingManager::instance().on_tick(1000);
    EXPECT_EQ(ChargingManager::instance().get_soc(), 100);
    
    // 3.3V 及以下，电量必须是 0%
    mock->set_voltage(3300);
    ChargingManager::instance().on_tick(1000);
    EXPECT_EQ(ChargingManager::instance().get_soc(), 0);
    
    mock->set_voltage(3000);
    ChargingManager::instance().on_tick(1000);
    EXPECT_EQ(ChargingManager::instance().get_soc(), 0);
}

TEST_F(ChargingManagerTest, CalculateSocLinearInterpolation) {
    MockBatteryDriver* mock = ChargingManager::instance().get_mock_driver();
    
    // 3800mV 应该是 50%
    mock->set_voltage(3800);
    ChargingManager::instance().on_tick(1000);
    EXPECT_EQ(ChargingManager::instance().get_soc(), 50);
    
    // 测试上半段：3975mV (位于 3800 和 4150 的中点) -> 应该约等于 75%
    mock->set_voltage(3975);
    ChargingManager::instance().on_tick(1000);
    EXPECT_EQ(ChargingManager::instance().get_soc(), 75);
    
    // 测试下半段：3550mV (位于 3300 和 3800 的中点) -> 应该约等于 25%
    mock->set_voltage(3550);
    ChargingManager::instance().on_tick(1000);
    EXPECT_EQ(ChargingManager::instance().get_soc(), 25);
}

TEST_F(ChargingManagerTest, PlugEdgeDetection) {
    MockBatteryDriver* mock = ChargingManager::instance().get_mock_driver();
    
    EXPECT_FALSE(ChargingManager::instance().is_plugged());
    
    // 模拟插入充电线
    mock->set_plugged(true);
    ChargingManager::instance().on_tick(1000);
    
    // 应该产生一个刚插入的上升沿脉冲
    EXPECT_TRUE(ChargingManager::instance().is_plugged());
    EXPECT_TRUE(ChargingManager::instance().has_just_plugged());
    EXPECT_FALSE(ChargingManager::instance().has_just_unplugged());
    
    // 下一个 tick (再过1秒) 上升沿应该被清除
    ChargingManager::instance().on_tick(1000);
    EXPECT_TRUE(ChargingManager::instance().is_plugged());
    EXPECT_FALSE(ChargingManager::instance().has_just_plugged());
    
    // 模拟拔出充电线
    mock->set_plugged(false);
    ChargingManager::instance().on_tick(1000);
    
    // 应该产生一个刚拔出的下降沿脉冲
    EXPECT_FALSE(ChargingManager::instance().is_plugged());
    EXPECT_TRUE(ChargingManager::instance().has_just_unplugged());
    EXPECT_FALSE(ChargingManager::instance().has_just_plugged());
}

TEST_F(ChargingManagerTest, PowerManagerCriticalProtection) {
    MockBatteryDriver* mock = ChargingManager::instance().get_mock_driver();
    
    // 手动重置 PowerManager 到 ACTIVE
    PowerManager::instance().transition_to(PowerState::ACTIVE);
    
    // 模拟电量极低 (< 5%) 且未充电
    mock->set_voltage(3300); // -> 0%
    mock->set_plugged(false);
    
    // PowerManager 级联轮询（需要1000ms让ChargingManager生效，因为我们测试用的是1000ms间隔）
    PowerManager::instance().on_tick(1000);
    
    // 此时 PowerManager 必须主动切断总线并陷入 CRITICAL 状态
    EXPECT_EQ(PowerManager::instance().get_state(), PowerState::CRITICAL);
}

TEST_F(ChargingManagerTest, PlugInWakesUpPowerManager) {
    MockBatteryDriver* mock = ChargingManager::instance().get_mock_driver();
    
    // 手动重置 PowerManager 到 IDLE
    PowerManager::instance().transition_to(PowerState::IDLE);
    
    // 模拟拔出状态下电量健康
    mock->set_voltage(4000); 
    mock->set_plugged(false);
    PowerManager::instance().on_tick(1000);
    EXPECT_EQ(PowerManager::instance().get_state(), PowerState::IDLE);
    
    // 模拟突然插入充电器
    mock->set_plugged(true);
    PowerManager::instance().on_tick(1000);
    
    // PowerManager 必须因为刚刚插入的事件立刻拉起系统到 ACTIVE（亮屏提示充电）
    EXPECT_EQ(PowerManager::instance().get_state(), PowerState::ACTIVE);
}

TEST_F(ChargingManagerTest, WristWakeDetectorIntegration) {
    MockBatteryDriver* mock = ChargingManager::instance().get_mock_driver();
    
    // Ensure accelerometer is powered on so read() works
    SensorManager::instance().get_accel_sensor().power_up();
    
    // Ensure sufficient battery
    mock->set_voltage(4000); 
    mock->set_plugged(false);
    
    // Start at IDLE
    PowerManager::instance().transition_to(PowerState::IDLE);
    
    // Feed IMU data: screen is facing up, but movement is unstable initially
    for (int i = 0; i < 3; i++) {
        SensorManager::instance().get_accel_sensor().set_mock_data(0, 0, 980);
        PowerManager::instance().on_tick(30);
    }
    
    // State should remain IDLE until steady ticks accumulate
    EXPECT_EQ(PowerManager::instance().get_state(), PowerState::IDLE);
    
    // Feed stable facing-up data for 1200ms (40 frames of 30ms)
    for (int i = 0; i < 40; i++) {
        SensorManager::instance().get_accel_sensor().set_mock_data(0, 0, 980);
        PowerManager::instance().on_tick(30);
    }
    
    // PowerManager should now wake up to ACTIVE due to wrist raise
    EXPECT_EQ(PowerManager::instance().get_state(), PowerState::ACTIVE);
    
    // Now simulate arm dropping (Z goes negative or zero, X/Y gets gravity)
    for (int i = 0; i < 20; i++) {
        SensorManager::instance().get_accel_sensor().set_mock_data(980, 0, 0);
        PowerManager::instance().on_tick(30);
    }
    
    // Still ACTIVE because IDLE timeout hasn't occurred yet! 
    // It shouldn't instantly sleep when arm drops, but rely on timeout,
    // OR it could dim immediately. Let's see what the system does...
    // The current mock just expects normal timeout logic for falling back.
}

// =============================================================================
// WristWakeDetector reset regression tests
//
// Reproduces the bug where steady_ticks_ accumulated during one IDLE/SLEEP
// window could carry over into the next, causing a spurious wrist-wake on the
// very first tick even though the wrist was never held steady for a full second.
// =============================================================================

TEST_F(ChargingManagerTest, WristWakeDetector_NoSpuriousWakeAfterIdleActiveIdle) {
    MockBatteryDriver* mock = ChargingManager::instance().get_mock_driver();
    SensorManager::instance().get_accel_sensor().power_up();

    mock->set_voltage(4000);
    mock->set_plugged(false);

    // ── Phase 1: Go to IDLE and accumulate 900 ms of steady wrist data
    //    (below the 1000 ms trigger threshold).
    PowerManager::instance().transition_to(PowerState::IDLE);
    for (int i = 0; i < 30; i++) { // 30 × 30 ms = 900 ms
        SensorManager::instance().get_accel_sensor().set_mock_data(0, 0, 980);
        PowerManager::instance().on_tick(30);
    }
    EXPECT_EQ(PowerManager::instance().get_state(), PowerState::IDLE);

    // ── Phase 2: External event (e.g. button press) forces ACTIVE.
    //    transition_to() must call wake_detector_.reset() here.
    PowerManager::instance().transition_to(PowerState::ACTIVE);
    EXPECT_EQ(PowerManager::instance().get_state(), PowerState::ACTIVE);

    // ── Phase 3: Re-enter IDLE (e.g. timeout).
    PowerManager::instance().transition_to(PowerState::IDLE);

    // ── Phase 4: Feed a single short tick with wrist-up pose.
    //    If steady_ticks_ was NOT reset, the leftover 900 ms + 30 ms = 930 ms
    //    would still be below 1000 ms, so no wake yet.  But if we feed 100 ms
    //    it would cross 1000 ms and fire — proving residual state carries over.
    //    After the fix, the detector starts fresh and 100 ms alone is < 1000 ms,
    //    so the system must stay in IDLE.
    SensorManager::instance().get_accel_sensor().set_mock_data(0, 0, 980);
    PowerManager::instance().on_tick(100);

    // Must still be IDLE — 100 ms of stable data is not enough to wake.
    EXPECT_EQ(PowerManager::instance().get_state(), PowerState::IDLE);
}

TEST_F(ChargingManagerTest, WristWakeDetector_NoSpuriousWakeAfterSleepActiveSleep) {
    MockBatteryDriver* mock = ChargingManager::instance().get_mock_driver();
    SensorManager::instance().get_accel_sensor().power_up();

    mock->set_voltage(4000);
    mock->set_plugged(false);

    // ── Phase 1: Go to SLEEP and accumulate 900 ms of steady wrist data.
    PowerManager::instance().transition_to(PowerState::SLEEP);
    for (int i = 0; i < 30; i++) { // 30 × 30 ms = 900 ms
        SensorManager::instance().get_accel_sensor().set_mock_data(0, 0, 980);
        PowerManager::instance().on_tick(30);
    }
    EXPECT_EQ(PowerManager::instance().get_state(), PowerState::SLEEP);

    // ── Phase 2: Charge cable plugged in, forces ACTIVE.
    mock->set_plugged(true);
    PowerManager::instance().on_tick(1000); // triggers has_just_plugged() path
    EXPECT_EQ(PowerManager::instance().get_state(), PowerState::ACTIVE);

    // ── Phase 3: Re-enter SLEEP.
    PowerManager::instance().transition_to(PowerState::SLEEP);

    // ── Phase 4: Feed 100 ms with wrist-up pose.
    //    Must NOT trigger wake — the detector must have been reset.
    SensorManager::instance().get_accel_sensor().set_mock_data(0, 0, 980);
    PowerManager::instance().on_tick(100);

    EXPECT_EQ(PowerManager::instance().get_state(), PowerState::SLEEP);
}

