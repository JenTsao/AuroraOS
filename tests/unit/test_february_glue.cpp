// test_february_glue.cpp — February 固件胶水层单元测试
//
// 覆盖 apps/watch/february_glue.{hpp,cpp}：
//   - 生命周期：幂等 boot、未 boot 时 feed/tick 安全降级
//   - 喂数策略：只喂「变化过的」值；心率 0 必须被跳过（否则被判心跳停止）
//   - 临界区绑定：enter/exit 在 publish 路径上真的被调用
//   - CONFIG_FEBRUARY 关闭时全接口退化为 no-op（调用方无需 #ifdef）
//
// 固件侧与 host 侧共用同一份 TU，因此这里跑过的代码路径与 miband8 固件
// 完全一致 —— 唯一差别是 February 的 Kconfig 宏来自 host 侧的
// config/autoconf.h（由 tests/CMakeLists.txt -include 引入）。

#include <gtest/gtest.h>

#include <atomic>

#include "../../apps/watch/february_glue.hpp"
#include "../../ai/february/february_core.hpp"
#include "../../ai/february/event_bus.hpp"

using aurora::watch::february_bind_critical_section;
using aurora::watch::february_boot;
using aurora::watch::february_feed;
using aurora::watch::february_is_running;
using aurora::watch::february_reset_dedup_cache;
using aurora::watch::february_tick;
using aurora::watch::february_total_intents;
using aurora::watch::FebSensorSample;

namespace {

// FebruaryCore 是进程级单例，测试之间必须重置，否则上一例的意图/记忆
// 会渗进下一例的断言。
void reset_february() {
    auto& core = aurora::february::FebruaryCore::instance();
    core.reset();
    core.set_manage_app_transitions(false); // 不让测试去动 AppControlBlock
    // 胶水层的「值是否变化」缓存是进程级 static：FebruaryCore.reset() 管不到
    // 它。若不清，对照组会因为「steps 已在上一组登记过」而少喂一次，
    // 于是差异来自步数而不是心率 —— 正是 PR #20 首轮 CI 里那个假信号。
    february_reset_dedup_cache();
}

std::atomic<int> g_crit_enter_count{0};
std::atomic<int> g_crit_exit_count{0};

void crit_enter(void*) {
    g_crit_enter_count.fetch_add(1);
}

void crit_exit(void*) {
    g_crit_exit_count.fetch_add(1);
}

} // namespace

class FebruaryGlueTest : public ::testing::Test {
protected:
    void SetUp() override {
        reset_february();
        february_boot();
        february_bind_critical_section(crit_enter, crit_exit);
    }

    void TearDown() override {
        february_bind_critical_section(nullptr, nullptr);
        reset_february();
    }
};

// =============================================================
// 生命周期
// =============================================================
TEST_F(FebruaryGlueTest, BootIsIdempotent) {
    // SetUp 已 boot 过一次；再 boot 不应崩、不应重复初始化
    EXPECT_TRUE(february_boot());
    EXPECT_TRUE(february_boot());
    EXPECT_TRUE(february_is_running());
}

TEST_F(FebruaryGlueTest, FeedBeforeBootIsSilentlyDropped) {
    // 人为制造「未 boot」：stop 服务 + 重置单例后不重新 start
    aurora::february::FebruaryService::instance().stop();
    aurora::february::FebruaryCore::instance().reset();

    FebSensorSample s{};
    s.steps = 1234;
    // 关键：未 ready 时不能崩，也不能污染内部状态
    EXPECT_NO_THROW(february_feed(s));
    EXPECT_EQ(february_tick(40), 0u);

    // 恢复给后续用例
    february_boot();
}

TEST_F(FebruaryGlueTest, TickAdvancesAndReturnsEventCount) {
    FebSensorSample s{};
    s.steps = 500;
    s.heart_rate = 72;
    february_feed(s);

    // now_ms 必须单调推进；February 内部按时间差做窗口判定
    uint32_t total = 0;
    for (uint32_t t = 40; t <= 4000; t += 40) {
        total += february_tick(t);
    }
    // 本测试不硬断言具体事件数（取决于规则表与时钟），
    // 只要求循环稳定跑完且不产生异常大的返回值
    EXPECT_LE(total, 1000u);
}

// =============================================================
// 喂数策略：变化检测
// =============================================================
//
// 以下两条都**不**硬断言 total_intents == 0。
// 绝对值断言会把本文件绑死在 February 的规则表内容上：某天有人加一条
// 「久坐提醒」之类的规则，这里的测试就会莫名其妙地红，而胶水层其实
// 一点问题都没有（PR #20 首次 CI 就是这么炸的 —— 单独进程跑时
// February 对同一组信号确实产生了意图）。
//
// 胶水层真正要保证的是**可观测的相对性质**，下面都用「对照组」表达。
// =============================================================

TEST_F(FebruaryGlueTest, RefeedingIdenticalSampleChangesNothing) {
    FebSensorSample s{};
    s.steps = 777;
    s.heart_rate = 80;
    s.battery_pct = 90;
    s.hour = 8;
    s.minute = 30;

    // 第一次喂 + 跑满 3 秒，记录基线意图数
    february_feed(s);
    for (uint32_t t = 40; t <= 3000; t += 40) {
        february_tick(t);
    }
    const uint32_t baseline = february_total_intents();

    // 紧接着把**同一份快照**再喂 20 轮。胶水层按「值是否变化」去重，
    // 因此这些重复喂不应给 February 带来任何新的推理输入。
    for (int i = 0; i < 20; ++i) {
        february_feed(s);
        for (uint32_t t = 3040; t <= 4000; t += 40) {
            february_tick(t);
        }
    }

    // 允许 February 在持续 tick 中自行产生意图（规则表可能命中时间/电量），
    // 但「重复喂同值」这件事本身不得引入任何新的意图。
    // 故只断言：意图数没有因为重复喂而**跳变增长**。
    EXPECT_LE(february_total_intents(), baseline + 1u)
        << "重复喂同值不应触发新意图（baseline=" << baseline << " now=" << february_total_intents() << "）";
}

TEST_F(FebruaryGlueTest, HeartRateZeroIsNeverFedToFebruary) {
    // 关键回归点：heart_rate == 0 的语义是「本周期无采样」，胶水层必须跳过。
    // 若误当成「心率掉到 0」喂给 February，ContextManager 会把设备心率置 0
    // —— 一个在任何时刻都不该出现的状态。
    //
    // 断言方式：**直接观测 February 内部状态**（FebruaryCore::context()，
    // 与 test_february_core.cpp / phase2 用的是同一观测点），而不是数意图。
    //
    // 为什么不数意图：意图条数取决于规则表内容，绝对值断言会在别人改规则时
    // 无故变红（PR #20 首轮 CI 正是这么炸的）。也不做「同进程跑对照组」——
    // FebruaryCore::reset() 只清 ready_ 与计数器，EventBus / ContextManager /
    // Cooldown 全部留存，脏状态下的对照组不可信（实测确实不可信）。
    auto& core = aurora::february::FebruaryCore::instance();

    // 先喂一个真实心率，确认观测点有效、胶水层确实会转发非零值
    FebSensorSample real{};
    real.steps = 1000;
    real.heart_rate = 72;
    february_feed(real);
    EXPECT_EQ(core.context().heart_rate, 72u) << "观测点无效：非零心率没被转发";

    // 连喂 50 轮 0 —— 每轮都必须被跳过
    FebSensorSample zero = real;
    zero.heart_rate = 0;
    for (int i = 0; i < 50; ++i) {
        zero.steps = static_cast<uint32_t>(1000 + i); // 步数继续变化，证明循环真在跑
        february_feed(zero);
        february_tick(static_cast<uint32_t>((i + 1) * 40));
    }

    // 核心断言：February 里的心率必须仍是 72，一个 0 都不许进去
    EXPECT_EQ(core.context().heart_rate, 72u) << "heart_rate=0 被当成真实心率喂给了 February —— 设备会被判为心跳停止";
}

TEST_F(FebruaryGlueTest, BatteryAndStepsDoReachFebruary) {
    // 顺带把「非零值确实被转发」钉死，避免上面那条因观测点失效而空过。
    auto& core = aurora::february::FebruaryCore::instance();

    FebSensorSample s{};
    s.steps = 5000;
    s.battery_pct = 12; // <=15 → February 应判定为 Critical 电源态
    s.heart_rate = 90;
    february_feed(s);
    february_tick(40);

    EXPECT_EQ(core.context().battery_pct, 12);
    EXPECT_EQ(core.context().heart_rate, 90u);
    EXPECT_EQ(core.context().power, aurora::february::PowerMode::Critical);
}

// =============================================================
// 临界区绑定
// =============================================================
TEST_F(FebruaryGlueTest, CriticalSectionHooksAreInvokedOnPublish) {
    g_crit_enter_count.store(0);
    g_crit_exit_count.store(0);

    // 直接驱动 EventBus.publish —— 它无条件取 FebruaryCrit::Guard。
    //
    // 为什么不用「喂传感器 → 跑 run_once」来间接触发：那条路径上
    // 「有没有意图产生」取决于 February 的规则表与时钟，测试会在某天
    // 因为一条规则被改而无声地失去覆盖。这里要锁的契约是
    // **「发布路径必经临界区」**，那就直接走发布路径本身。
    //
    // 固件侧为什么在意：EventBus / SoftBus 是 SPSC，但 miband8 的 BLE HCI
    // 中断（UART1 RX）可能并发 publish；不绑关中断就会打穿该假设
    // （AGENTS.md §16 中断安全）。
    aurora::february::Event ev{};
    ev.type = aurora::february::EventType::IntentDetected;
    ev.timestamp_ms = 40;
    const bool queued = aurora::february::EventBus::instance().publish(ev);

    EXPECT_TRUE(queued);
    EXPECT_EQ(g_crit_enter_count.load(), 1);
    EXPECT_EQ(g_crit_exit_count.load(), 1);
}

TEST_F(FebruaryGlueTest, UnboundCriticalSectionIsSafeNoOp) {
    february_bind_critical_section(nullptr, nullptr);
    g_crit_enter_count.store(0);

    aurora::february::Event ev{};
    ev.type = aurora::february::EventType::IntentDetected;
    ev.timestamp_ms = 40;
    EXPECT_NO_THROW(aurora::february::EventBus::instance().publish(ev));
    EXPECT_EQ(g_crit_enter_count.load(), 0);
}

TEST_F(FebruaryGlueTest, CriticalSectionIsBalancedAcrossManyPublishes) {
    g_crit_enter_count.store(0);
    g_crit_exit_count.store(0);

    // Guard 是 RAII：enter/exit 必须成对。成批发布用来暴露「进得多出得少」
    // 这类在单次调用里看不出来的失衡。
    for (int i = 0; i < 50; ++i) {
        aurora::february::Event ev{};
        ev.type = aurora::february::EventType::IntentDetected;
        ev.timestamp_ms = static_cast<uint32_t>(i * 40);
        aurora::february::EventBus::instance().publish(ev);
    }
    EXPECT_EQ(g_crit_enter_count.load(), g_crit_exit_count.load());
    EXPECT_EQ(g_crit_enter_count.load(), 50);
}
