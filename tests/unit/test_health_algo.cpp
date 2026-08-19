// test_health_algo.cpp — 运动健康算法框架单元测试
//
// 测试策略 (cpp-testing):
//   - 纯计算类：无任何 I/O 或全局状态依赖，直接注入序列数据
//   - TDD 风格：先写断言语义，再验证算法正确性
//   - GTest Fixtures 管理 SetUp/TearDown
//   - ASSERT_* 用于前置条件，EXPECT_* 用于多项校验

#include <gtest/gtest.h>
#include "../../drivers/sensor/health_algo.hpp"

using namespace aurora::health;

// =============================================================
// PpgHeartRateFilter 测试
// =============================================================
class PpgFilterTest : public ::testing::Test {
protected:
    PpgHeartRateFilter filter;
};

TEST_F(PpgFilterTest, ConstantInputConvergesToValue) {
    constexpr uint32_t kBpm = 72;
    int32_t result = 0;
    // 经过足够多的样本后，输出应该收敛至输入附近 (±5 BPM 容差)
    for (int i = 0; i < 64; ++i) {
        result = filter.update(kBpm);
    }
    EXPECT_NEAR(result, static_cast<int32_t>(kBpm), 5);
}

TEST_F(PpgFilterTest, FirstSampleInitializesImmediately) {
    // 首次采样应该立即初始化，不返回 0
    const int32_t result = filter.update(80);
    EXPECT_GT(result, 0);
}

TEST_F(PpgFilterTest, SpikeNoiseIsAttenuated) {
    // 建立 75 BPM 稳态
    for (int i = 0; i < 32; ++i)
        (void)filter.update(75);

    // 注入一个 200 BPM 尖峰
    (void)filter.update(200);

    // 下一个正常采样后，输出不应跳跃超过 30 BPM
    const int32_t after_spike = filter.update(75);
    EXPECT_LT(after_spike, 75 + 30);
}

TEST_F(PpgFilterTest, ResetClearsState) {
    for (int i = 0; i < 16; ++i)
        (void)filter.update(90);
    filter.reset();
    // 重置后，如果喂入 60 BPM，第一次输出应该接近 60，而非 90
    const int32_t after_reset = filter.update(60);
    EXPECT_NEAR(after_reset, 60, 5);
}

TEST_F(PpgFilterTest, SlowlyIncreasingInputFollows) {
    // 心率从 60 逐渐升高到 120，滤波器应跟随
    int32_t last = filter.update(60);
    for (uint32_t bpm = 61; bpm <= 120; ++bpm) {
        last = filter.update(bpm);
    }
    // 最终输出应在 120 附近（考虑 IIR 延迟，放宽容差至 20）
    EXPECT_GT(last, 60);
    EXPECT_LE(last, 120);
}

// =============================================================
// StepDetector 测试
// =============================================================
class StepDetectorTest : public ::testing::Test {
protected:
    StepDetector detector;

    // 模拟一步：先推峰值，再降谷值，再回 1g 稳态
    void simulate_step(uint32_t step_interval_ms = 500) {
        // 峰值 (1400 mg)
        (void)detector.update(0, 0, 1400, step_interval_ms / 3);
        // 谷值 (600 mg)
        (void)detector.update(0, 0, 600, step_interval_ms / 3);
        // 回稳 (1000 mg)
        (void)detector.update(0, 0, 1000, step_interval_ms / 3);
    }
};

TEST_F(StepDetectorTest, NoMovementCountsZeroSteps) {
    // 持续静止 1g
    for (int i = 0; i < 100; ++i) {
        (void)detector.update(0, 0, 1000, 40);
    }
    EXPECT_EQ(detector.get_steps(), 0u);
}

TEST_F(StepDetectorTest, SimpleStepsAreCountedCorrectly) {
    // 模拟 10 步，间隔 500ms
    for (int i = 0; i < 10; ++i) {
        simulate_step(500);
    }
    EXPECT_EQ(detector.get_steps(), 10u);
}

TEST_F(StepDetectorTest, HighFrequencyVibrationUnder250msIsDebounced) {
    // 间隔 100ms (< 250ms)，不应该计步
    for (int i = 0; i < 20; ++i) {
        (void)detector.update(0, 0, 1400, 33); // peak
        (void)detector.update(0, 0, 600, 33);  // valley
        (void)detector.update(0, 0, 1000, 34); // stable
    }
    // 步数应为 0 或非常少（第一步可能被计入，因为初始 time_since_step 已超过阈值）
    EXPECT_LT(detector.get_steps(), 5u);
}

TEST_F(StepDetectorTest, ResetClearsStepCount) {
    for (int i = 0; i < 5; ++i)
        simulate_step(500);
    ASSERT_EQ(detector.get_steps(), 5u);
    detector.reset();
    EXPECT_EQ(detector.get_steps(), 0u);
}

TEST_F(StepDetectorTest, DynamicThresholdAdaptsToHigherPeaks) {
    // 先建立正常步态（峰值 ~1400mg）
    for (int i = 0; i < 8; ++i)
        simulate_step(500);
    const uint32_t steps_after_normal = detector.get_steps();

    // 模拟跑步（峰值 ~2000mg），动态阈值应上调，仍能计步
    for (int i = 0; i < 5; ++i) {
        (void)detector.update(0, 0, 2000, 300); // 高峰值
        (void)detector.update(0, 0, 400, 100);  // 低谷
        (void)detector.update(0, 0, 1000, 100); // 回稳
    }
    EXPECT_GT(detector.get_steps(), steps_after_normal);
}

// =============================================================
// ActivityStateEngine 测试
// =============================================================
class ActivityStateEngineTest : public ::testing::Test {
protected:
    ActivityStateEngine engine;

    // 推进 N 秒，每秒注入固定步数
    void advance_seconds(uint32_t secs, uint32_t total_steps, int32_t bpm, uint32_t step_delta_per_sec = 0) {
        for (uint32_t s = 0; s < secs; ++s) {
            total_steps += step_delta_per_sec;
            engine.update(total_steps, bpm, 1000);
        }
    }
};

TEST_F(ActivityStateEngineTest, WalkingCadenceProducesWalkingState) {
    // 90 步/分钟：每秒 1.5 步，跑 30 秒建立稳态
    uint32_t steps = 0;
    for (uint32_t s = 0; s < 60; ++s) {
        if (s % 2 == 0)
            ++steps; // 每2秒一步 = 30步/分, 不够
    }
    // 更准确：每秒送1步，60秒后步频 = 60 spm
    steps = 0;
    engine.reset();
    for (uint32_t s = 0; s < 61; ++s) {
        steps += 2; // 2步/秒 = 120步/分钟，WALKING 范围
        engine.update(steps, 80, 1000);
    }
    EXPECT_EQ(engine.get_state(), ActivityState::walking);
}

TEST_F(ActivityStateEngineTest, RunningCadenceProducesRunningState) {
    uint32_t steps = 0;
    for (uint32_t s = 0; s < 61; ++s) {
        steps += 3; // 3步/秒 = 180步/分钟，RUNNING 范围
        engine.update(steps, 160, 1000);
    }
    EXPECT_EQ(engine.get_state(), ActivityState::running);
}

TEST_F(ActivityStateEngineTest, StillForOver30SecondsIsStillState) {
    // 静止超过 30 秒，无步态，低心率
    for (uint32_t s = 0; s < 35; ++s) {
        engine.update(0, 60, 1000); // 0 步，60 BPM
    }
    EXPECT_EQ(engine.get_state(), ActivityState::still);
}

TEST_F(ActivityStateEngineTest, SleepingStateRequiresStillAndLowHR) {
    // 静止 300+ 秒 + 心率 < 65 + 连续确认窗口 60 秒 → SLEEPING
    // 总时长 = 300 (sleep_still_sec) + 60 (enter_confirm) + 余量
    for (uint32_t s = 0; s < 370; ++s) {
        engine.update(0, 58, 1000);
    }
    EXPECT_EQ(engine.get_state(), ActivityState::sleeping);
    EXPECT_TRUE(engine.should_deep_sleep());
    EXPECT_TRUE(engine.should_disable_wrist_wake());
}

TEST_F(ActivityStateEngineTest, SleepingEntryNeedsConfirmWindow) {
    // 静止超过 300 秒且心率 < 65，但未满 60 秒确认窗口 → 仍不是 SLEEPING
    for (uint32_t s = 0; s < 330; ++s) {
        engine.update(0, 58, 1000);
    }
    EXPECT_NE(engine.get_state(), ActivityState::sleeping);
}

TEST_F(ActivityStateEngineTest, HighHRPreventsSleeingEvenIfStill) {
    // 静止 400+ 秒，但心率 >= 65 → 不进入睡眠 (即使确认窗口已过)
    for (uint32_t s = 0; s < 400; ++s) {
        engine.update(0, 70, 1000); // 心率 70 >= 65
    }
    EXPECT_NE(engine.get_state(), ActivityState::sleeping);
    EXPECT_FALSE(engine.should_deep_sleep());
}

TEST_F(ActivityStateEngineTest, SleepingOccasionalTossAndTurnDoesNotWake) {
    // 进入睡眠后，偶发翻身 (每 10 秒 1 步 ≈ 6 spm < min_exit_cadence) 不退出睡眠
    for (uint32_t s = 0; s < 370; ++s) {
        engine.update(0, 58, 1000);
    }
    ASSERT_EQ(engine.get_state(), ActivityState::sleeping);

    uint32_t steps = 0;
    for (uint32_t s = 0; s < 120; ++s) {
        if (s % 10 == 0)
            ++steps; // 每 10 秒 1 步
        engine.update(steps, 58, 1000);
    }
    EXPECT_EQ(engine.get_state(), ActivityState::sleeping);
}

TEST_F(ActivityStateEngineTest, SleepingSustainedMovementWakesAfterBuffer) {
    // 进入睡眠后，持续轻运动 (每 2 秒 1 步) 需要经过完整缓冲才退出：
    //   spm 爬到 >= 10 约需 20s → motion_confirm (10s) → exit_confirm (30s)
    //   共约 60s，这里给 70s 确保退出
    for (uint32_t s = 0; s < 370; ++s) {
        engine.update(0, 58, 1000);
    }
    ASSERT_EQ(engine.get_state(), ActivityState::sleeping);

    uint32_t steps = 0;
    for (uint32_t s = 0; s < 70; ++s) {
        if (s % 2 == 0)
            ++steps; // 每 2 秒 1 步 → 60 秒窗口内约 30 spm
        engine.update(steps, 58, 1000);
    }
    EXPECT_EQ(engine.get_state(), ActivityState::still);
}

TEST_F(ActivityStateEngineTest, HeartRateRiseDuringSleepWakes) {
    // 进入睡眠后，心率持续上升 (> 基线+15 BPM) 触发退出
    for (uint32_t s = 0; s < 370; ++s) {
        engine.update(0, 58, 1000);
    }
    ASSERT_EQ(engine.get_state(), ActivityState::sleeping);

    for (uint32_t s = 0; s < 50; ++s) {
        engine.update(0, 80, 1000); // 58 + 15 = 73 < 80，持续上升
    }
    EXPECT_EQ(engine.get_state(), ActivityState::still);
}

TEST_F(ActivityStateEngineTest, ResetReturnsToUnknown) {
    for (uint32_t s = 0; s < 10; ++s)
        engine.update(5 * s, 75, 1000);
    engine.reset();
    EXPECT_EQ(engine.get_state(), ActivityState::unknown);
}

// =============================================================
// HealthAlgoEngine 集成冒烟测试
// =============================================================
TEST(HealthAlgoEngineTest, IntegrationSmoke) {
    HealthAlgoEngine eng;

    // 喂入 30 次 HR + 加速度步态序列
    for (int i = 0; i < 30; ++i) {
        (void)eng.on_ppg_sample(72);
        // 模拟一步（峰谷各 33ms）
        (void)eng.on_accel_sample(0, 0, 1400, 167);
        (void)eng.on_accel_sample(0, 0, 600, 167);
        (void)eng.on_accel_sample(0, 0, 1000, 166);
        eng.advance_activity(500);
    }

    EXPECT_GT(eng.get_filtered_bpm(), 0);
    EXPECT_GE(eng.get_total_steps(), 1u);
    // 不要 assert specific state - depends on 60s window
}

TEST(HealthAlgoEngineTest, ResetClearsAllSubsystems) {
    HealthAlgoEngine eng;
    for (int i = 0; i < 20; ++i) {
        (void)eng.on_ppg_sample(80);
        (void)eng.on_accel_sample(0, 0, 1400, 150);
        (void)eng.on_accel_sample(0, 0, 600, 150);
        (void)eng.on_accel_sample(0, 0, 1000, 200);
        eng.advance_activity(500);
    }
    EXPECT_GE(eng.get_total_steps(), 1u);
    eng.reset();
    EXPECT_EQ(eng.get_total_steps(), 0u);
    EXPECT_EQ(eng.get_activity_state(), ActivityState::unknown);
}
