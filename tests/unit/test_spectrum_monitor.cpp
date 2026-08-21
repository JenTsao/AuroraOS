// test_spectrum_monitor.cpp — 射频频谱感知守护引擎单元测试
//
// 覆盖 SpectrumMonitor 集成层：
//   - 传感器绑定与噪声底校准
//   - 宽带压制干扰识别 + SecurityMonitor 联动
//   - 连续波干扰识别（跨帧分类）
//   - 瞬时突发异常识别（SuddenBurst）

#include <gtest/gtest.h>
#include "../../drivers/rf/spectrum_monitor.hpp"

using namespace aurora::rf;

// 测试环境使用 kernel_stubs.cpp 中的系统 tick 计数器
extern volatile uint32_t tick_count;

class SpectrumMonitorTest : public ::testing::Test {
protected:
    void SetUp() override {
        tick_count = 1000;
        mon_ = &SpectrumMonitor::instance();
        mon_->reset();
        mon_->set_report_cooldown_ms(0); // 每次告警都上报，便于断言 SecurityMonitor 联动
        sensor_.configure(2400, 1000, 16);
        sensor_.power_up();
        mon_->init(&sensor_);
    }

    // 连续推进 frames 次扫频 + 分析
    void feed(int frames) {
        SpectrumSweep sweep;
        for (int i = 0; i < frames; ++i) {
            ASSERT_TRUE(sensor_.sweep(&sweep));
            mon_->process_sweep(sweep);
        }
    }

    // 在告警缓冲中查找指定来源 + 类型的告警
    bool has_alert(SpectrumAlertKind kind, uint8_t type) const {
        const uint32_t total = mon_->get_alert_count();
        for (uint32_t i = 0; i < total; ++i) {
            const SpectrumAlert* a = mon_->get_alert(static_cast<int>(i));
            if (a && a->kind == kind && a->type == type)
                return true;
        }
        return false;
    }

    MockSpectrumSensor sensor_;
    SpectrumMonitor* mon_ = nullptr;
};

TEST_F(SpectrumMonitorTest, BindsSensorAndCalibrates) {
    EXPECT_EQ(mon_->get_sensor(), &sensor_);
    EXPECT_FALSE(mon_->is_calibrated());

    sensor_.fill(-90); // 平坦噪声底
    feed(2);

    EXPECT_TRUE(mon_->is_calibrated());
    EXPECT_EQ(mon_->get_sweep_count(), 2u);
    EXPECT_EQ(power::to_dbm(mon_->get_noise_floor(0)), -90);
}

TEST_F(SpectrumMonitorTest, DetectsWidebandJammingAndReports) {
    sensor_.fill(-90);
    feed(2); // 建立基线

    const uint32_t alerts_before = mon_->get_alert_count();
    const uint32_t secmon_before = SecurityMonitor::instance().get_firewall_anomaly_count();

    // 全频带从 -90 抬升到 -50 → 宽带压制
    sensor_.fill(-50);
    feed(1);

    EXPECT_GT(mon_->get_alert_count(), alerts_before);
    EXPECT_TRUE(has_alert(SpectrumAlertKind::Jamming, static_cast<uint8_t>(JammingType::BroadbandNoise)));
    EXPECT_GT(SecurityMonitor::instance().get_firewall_anomaly_count(), secmon_before);
}

TEST_F(SpectrumMonitorTest, DetectsContinuousWaveJamming) {
    sensor_.fill(-90);
    feed(2);

    // 连续 3 帧在同一频点注入 CW → 连续波干扰
    sensor_.inject_cw(2405, -40);
    feed(3);

    EXPECT_TRUE(has_alert(SpectrumAlertKind::Jamming, static_cast<uint8_t>(JammingType::ContinuousWave)));
}

TEST_F(SpectrumMonitorTest, DetectsSuddenBurstAnomaly) {
    sensor_.fill(-90);
    feed(2);

    // 突然注入 -40 dBm 信号：瞬时跳变 → SuddenBurst 异常
    sensor_.inject_cw(2405, -40);
    feed(1);

    EXPECT_TRUE(has_alert(SpectrumAlertKind::Anomaly, static_cast<uint8_t>(AnomalyType::SuddenBurst)));
}

TEST_F(SpectrumMonitorTest, NoAlertOnCleanSpectrum) {
    sensor_.fill(-90);
    feed(3);

    EXPECT_EQ(mon_->get_alert_count(), 0u);
}
