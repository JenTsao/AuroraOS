// test_rf_spectrum.cpp — 射频频谱感知单元测试
//
// 覆盖：
//   - PowerQ8 定点换算
//   - MockSpectrumSensor 频谱注入
//   - RfAnalyzer 噪声底估计与异常检测（超噪声底/绝对上限/突发/宽带抬升）
//   - JammingDetector 干扰识别（连续波/窄带/宽带噪声/扫频/脉冲）

#include <gtest/gtest.h>
#include "../../drivers/rf/spectrum_sensor.hpp"
#include "../../drivers/rf/rf_analyzer.hpp"
#include "../../drivers/rf/jamming_detector.hpp"

using namespace aurora::rf;

// 统一测试频谱：2400 MHz 起始，16 分箱，步进 1 MHz
static void setup_sensor(MockSpectrumSensor& sensor) {
    sensor.configure(2400, 1000, 16);
    sensor.power_up();
}

// =============================================================
// PowerQ8 定点换算
// =============================================================
TEST(PowerQ8Test, ConvertRoundTrip) {
    EXPECT_EQ(power::from_dbm(-40), -10240);
    EXPECT_EQ(power::to_dbm(power::from_dbm(-40)), -40);
    EXPECT_EQ(power::to_dbm_tenths(power::from_dbm(-40)), -400);
    EXPECT_EQ(power::to_dbm_tenths(power::from_dbm(0)), 0);
}

// =============================================================
// MockSpectrumSensor
// =============================================================
TEST(MockSpectrumSensorTest, SweepRequiresPowerUp) {
    MockSpectrumSensor sensor;
    setup_sensor(sensor);
    sensor.power_down();

    SpectrumSweep sweep;
    EXPECT_FALSE(sensor.sweep(&sweep));
    sensor.power_up();
    EXPECT_TRUE(sensor.sweep(&sweep));
    EXPECT_EQ(sweep.bin_count, 16);
    EXPECT_EQ(sweep.start_freq_mhz, 2400);
    EXPECT_EQ(sweep.step_khz, 1000);
}

TEST(MockSpectrumSensorTest, InjectCwMapsToNearestBin) {
    MockSpectrumSensor sensor;
    setup_sensor(sensor);
    sensor.fill(-90);
    sensor.inject_cw(2405, -40);

    SpectrumSweep sweep;
    ASSERT_TRUE(sensor.sweep(&sweep));
    // 2405 MHz 对应分箱 5（2400 + 5*1MHz）
    EXPECT_EQ(power::to_dbm(sweep.bins[5].power_q8), -40);
    EXPECT_EQ(power::to_dbm(sweep.bins[0].power_q8), -90);
}

// =============================================================
// RfAnalyzer
// =============================================================
class RfAnalyzerTest : public ::testing::Test {
protected:
    void SetUp() override {
        setup_sensor(sensor);
        sensor.fill(-90); // 平坦噪声底
    }

    // 推进若干帧平坦底噪，建立噪声底基线
    void calibrate(int frames) {
        SpectrumSweep sweep;
        for (int i = 0; i < frames; ++i) {
            ASSERT_TRUE(sensor.sweep(&sweep));
            analyzer.analyze(sweep, nullptr, 0);
        }
    }

    MockSpectrumSensor sensor;
    RfAnalyzer analyzer;
};

TEST_F(RfAnalyzerTest, BuildsBaseline) {
    EXPECT_FALSE(analyzer.is_calibrated());
    calibrate(2);
    EXPECT_TRUE(analyzer.is_calibrated());
    EXPECT_EQ(power::to_dbm(analyzer.get_noise_floor(0)), -90);
}

TEST_F(RfAnalyzerTest, DetectsSuddenBurstThenAboveNoiseFloor) {
    calibrate(2);

    // 突然注入 -40 dBm 信号：瞬时跳变 50 dB → SuddenBurst
    sensor.inject_cw(2405, -40);
    SpectrumSweep sweep;
    ASSERT_TRUE(sensor.sweep(&sweep));
    AnomalyEvent events[8];
    int n = analyzer.analyze(sweep, events, 8);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(events[0].type, AnomalyType::SuddenBurst);

    // 信号持续：跳变消失，功率仍超噪声底 → AboveNoiseFloor
    ASSERT_TRUE(sensor.sweep(&sweep));
    n = analyzer.analyze(sweep, events, 8);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(events[0].type, AnomalyType::AboveNoiseFloor);
    EXPECT_EQ(power::to_dbm(events[0].power_q8), -40);
}

TEST_F(RfAnalyzerTest, DetectsAbsoluteHigh) {
    calibrate(2);

    // 注入 -10 dBm，超过绝对上限 -20 dBm → AbsoluteHigh
    sensor.inject_cw(2405, -10);
    SpectrumSweep sweep;
    ASSERT_TRUE(sensor.sweep(&sweep));
    AnomalyEvent events[8];
    int n = analyzer.analyze(sweep, events, 8);
    EXPECT_GE(n, 1);
    EXPECT_EQ(events[0].type, AnomalyType::AbsoluteHigh);
}

TEST_F(RfAnalyzerTest, DetectsWidebandRise) {
    calibrate(2);

    // 全频带从 -90 抬升到 -50 → WidebandRise（全局告警，抑制 bin 级事件）
    sensor.fill(-50);
    SpectrumSweep sweep;
    ASSERT_TRUE(sensor.sweep(&sweep));
    AnomalyEvent events[4];
    int n = analyzer.analyze(sweep, events, 4);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(events[0].type, AnomalyType::WidebandRise);
}

TEST_F(RfAnalyzerTest, NoAnomalyOnStableNoise) {
    calibrate(3);

    SpectrumSweep sweep;
    ASSERT_TRUE(sensor.sweep(&sweep));
    AnomalyEvent events[8];
    int n = analyzer.analyze(sweep, events, 8);
    EXPECT_EQ(n, 0);
}

// =============================================================
// JammingDetector
// =============================================================
class JammingDetectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        setup_sensor(sensor);
        sensor.fill(-90);
        detector.reset();
        analyzer.reset();
    }

    // 建立基线后返回当前扫频结果
    void calibrate(int frames) {
        SpectrumSweep sweep;
        for (int i = 0; i < frames; ++i) {
            ASSERT_TRUE(sensor.sweep(&sweep));
            analyzer.analyze(sweep, nullptr, 0);
        }
    }

    // 执行一次扫频 + 检测
    const JammingAlert* step() {
        SpectrumSweep sweep;
        if (!sensor.sweep(&sweep))
            return nullptr;
        return detector.detect(sweep);
    }

    MockSpectrumSensor sensor;
    RfAnalyzer analyzer;
    JammingDetector detector{analyzer};
};

TEST_F(JammingDetectorTest, DetectsBroadbandNoise) {
    calibrate(2);

    sensor.fill(-50); // 全频带抬升 → 宽带压制
    const JammingAlert* alert = step();
    ASSERT_NE(alert, nullptr);
    EXPECT_EQ(alert->type, JammingType::BroadbandNoise);
}

TEST_F(JammingDetectorTest, DetectsContinuousWave) {
    calibrate(2);

    // 连续 3 帧在同一频点注入 CW
    sensor.inject_cw(2405, -40);
    EXPECT_EQ(step()->type, JammingType::Narrowband); // 第 1 帧：历史不足
    EXPECT_EQ(step()->type, JammingType::Narrowband); // 第 2 帧
    const JammingAlert* alert = step();               // 第 3 帧：连续 3 帧同频
    ASSERT_NE(alert, nullptr);
    EXPECT_EQ(alert->type, JammingType::ContinuousWave);
    EXPECT_EQ(alert->center_freq_mhz, 2405);
}

TEST_F(JammingDetectorTest, DetectsSweepingChirp) {
    calibrate(2);

    // 峰值频率逐帧移动（每帧 2 MHz > 1 MHz 步进）
    sensor.inject_cw(2400, -40);
    EXPECT_EQ(step()->type, JammingType::Narrowband);

    sensor.fill(-90);
    sensor.inject_cw(2402, -40);
    EXPECT_EQ(step()->type, JammingType::Narrowband);

    sensor.fill(-90);
    sensor.inject_cw(2404, -40);
    const JammingAlert* alert = step();
    ASSERT_NE(alert, nullptr);
    EXPECT_EQ(alert->type, JammingType::SweepingChirp);
}

TEST_F(JammingDetectorTest, DetectsPulsed) {
    calibrate(2);

    // 活动 → 空 → 活动：脉冲模式
    sensor.inject_cw(2405, -40);
    EXPECT_EQ(step()->type, JammingType::Narrowband);

    sensor.fill(-90); // 空帧（无活动）
    EXPECT_EQ(step(), nullptr);

    sensor.inject_cw(2405, -40); // 再次活动
    const JammingAlert* alert = step();
    ASSERT_NE(alert, nullptr);
    EXPECT_EQ(alert->type, JammingType::Pulsed);
}

TEST_F(JammingDetectorTest, DetectsNarrowband) {
    calibrate(2);

    // 单帧局部窄带（跨越 3 个分箱，非单频 CW）
    sensor.inject_band(2404, 2406, -40);
    const JammingAlert* alert = step();
    ASSERT_NE(alert, nullptr);
    EXPECT_EQ(alert->type, JammingType::Narrowband);
    EXPECT_EQ(alert->center_freq_mhz, 2405); // 峰值在带内中心附近
}

TEST_F(JammingDetectorTest, NoAlertOnCleanSpectrum) {
    calibrate(3);

    const JammingAlert* alert = step();
    EXPECT_EQ(alert, nullptr);
}
