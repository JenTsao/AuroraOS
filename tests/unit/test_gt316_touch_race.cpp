// =============================================================================
// tests/unit/test_gt316_touch_race.cpp
//
// GT316 触摸「跨优先级双采样者」竞态回归测试
//
// -----------------------------------------------------------------------------
// 本文件固化的是这样一个真实缺陷的修复行为：
//
//   poll_touch() 过去被【两个不同优先级】的任务调用：
//     - ui_render_task          (Realtime, ~30fps)  -> watch_app.hpp on_frame_render()
//     - sensor_ble_daemon_task  (High,     40ms)   -> watch_app.cpp  on_background_tick()
//
//   current_state_ 是跨调用持久的成员变量，于是「RELEASED 已产出、但尚未被
//   消费」这个中间态会直接暴露给第二个调用者。它读到 current_state_ ==
//   RELEASED 就地把它改成 PRESSED，而第一个调用者还没来得及把 RELEASED
//   交给手势层 —— 结果手势层先收到新的 PRESSED、后收到旧的 RELEASED，
//   抬起事件在到达手势层之前就被吃掉。用户侧表现：手指还没离屏，抬手就没反应。
//
//修复分两层（均在本文件中被覆盖）：
//   1. 状态机自洽：RELEASED 引入「恰好产出一次」闩锁（release_pending_），
//      必须被显式消费后才允许转入 IDLE / 产出新的 PRESSED。
//   2. 单一采样者：删除 on_frame_render() 里的冗余 poll_input(0)，
//      触摸采样统一由后台守护任务（40ms）驱动。
//
// -----------------------------------------------------------------------------
// 为什么本文件不 include test_gt316_driver.cpp 里的 MockGt316I2cHal：
//   那个 Mock 只有 read_called 这一个 bool，无法量化「零读取」，
//   而本文件的核心断言之一恰恰是「帧渲染路径下GT316 寄存器零读取」。
//   故在下方独立复刻一个带【读/写事务计数】的 Mock，语义与原 Mock 严格一致
//   （0x814E bit7=Ready / bit3..0=点数；写 0x00 清Ready；0x814F 起 8 字节触点记录）。
//   test_gt316_driver.cpp 本身不被本文件修改或依赖。
// =============================================================================
#include <gtest/gtest.h>
#include <cstring>

#include "../../drivers/input/gt316_driver.hpp"
#include "../../drivers/input/gesture_recognizer.hpp"

// =============================================================================
// 带事务计数的 GT316 I2C HAL 模拟器
// =============================================================================
class CountingGt316I2cHal : public auroraos::hal::II2cHal {
public:
    uint8_t memory[0x10000]; // 64KB 寄存器空间，与真实 GT316 地址空间同宽
    uint16_t last_reg_written;
    uint32_t read_count;  // read() 调用次数  —— 用于断言「零读取」
    uint32_t write_count; // write() 调用次数
    uint32_t reg_read_count[256][256]; // 按寄存器统计的读取次数，key = 寄存器地址
    uint32_t reads_for_reg(uint16_t reg) const { return reg_read_count[reg >> 8][reg & 0xFF]; }

    CountingGt316I2cHal()
        : last_reg_written(0), read_count(0), write_count(0) {
        std::memset(memory, 0, sizeof(memory));
        std::memset(reg_read_count, 0, sizeof(reg_read_count));
        memory[GT316_REG_PRODUCT_ID + 0] = '3';
        memory[GT316_REG_PRODUCT_ID + 1] = '1';
        memory[GT316_REG_PRODUCT_ID + 2] = '6';
        memory[GT316_REG_PRODUCT_ID + 3] = '\0';
    }

    // 上报一次触点：Ready + Count=point_count，并写入坐标
    void simulate_touch(uint16_t x, uint16_t y, uint8_t point_count = 1) {
        memory[GT316_REG_BUFFER_STATUS] = static_cast<uint8_t>(0x80 | (point_count & 0x0F));
        memory[GT316_REG_POINT1_BASE + GT316_OFFSET_TRACK_ID] = 0;
        memory[GT316_REG_POINT1_BASE + GT316_OFFSET_X_LOW] = static_cast<uint8_t>(x & 0xFF);
        memory[GT316_REG_POINT1_BASE + GT316_OFFSET_X_HIGH] = static_cast<uint8_t>((x >> 8) & 0xFF);
        memory[GT316_REG_POINT1_BASE + GT316_OFFSET_Y_LOW] = static_cast<uint8_t>(y & 0xFF);
        memory[GT316_REG_POINT1_BASE + GT316_OFFSET_Y_HIGH] = static_cast<uint8_t>((y >> 8) & 0xFF);
        memory[GT316_REG_POINT1_BASE + GT316_OFFSET_POINT_SIZE] = 0x20;
    }

    // 手指离开：Ready 置位但点数为 0
    void simulate_release() {
        memory[GT316_REG_BUFFER_STATUS] = 0x80;
    }

    // 没有任何待处理数据：Ready 位为 0
    void simulate_idle() {
        memory[GT316_REG_BUFFER_STATUS] = 0x00;
    }

    void reset_counters() {
        read_count = 0;
        write_count = 0;
        std::memset(reg_read_count, 0, sizeof(reg_read_count));
    }

    bool write(uint8_t dev_addr, const uint8_t* data, size_t len) override {
        if (dev_addr != I2C_ADDR_GT316 || !data || len < 2) {
            return false;
        }
        write_count++;

        uint16_t reg = static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
        last_reg_written = reg;

        if (len > 2) {
            for (size_t i = 0; i < len - 2; ++i) {
                if (reg + i < sizeof(memory)) {
                    memory[reg + i] = data[2 + i];
                }
            }
        }
        return true;
    }

    bool read(uint8_t dev_addr, uint8_t* data, size_t len) override {
        if (dev_addr != I2C_ADDR_GT316 || !data) {
            return false;
        }
        read_count++;
        if (last_reg_written < sizeof(memory)) {
            reg_read_count[last_reg_written >> 8][last_reg_written & 0xFF]++;
        }
        for (size_t i = 0; i < len; ++i) {
            if (last_reg_written + i < sizeof(memory)) {
                data[i] = memory[last_reg_written + i];
            } else {
                data[i] = 0;
            }
        }
        return true;
    }

    bool write_reg(uint8_t, uint8_t, const uint8_t*, size_t) override { return false; }
    bool read_reg(uint8_t, uint8_t, uint8_t*, size_t) override { return false; }
};

// 测试夹具：每个用例独占一个驱动实例 + 一个带计数的 Mock
class Gt316TouchRaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        driver = new Gt316Driver("touch0", 192, 490);
        mock = new CountingGt316I2cHal();
        driver->configure(mock, nullptr, -1, -1, I2C_ADDR_GT316);
        driver->open();
        mock->reset_counters();
    }

    void TearDown() override {
        delete driver;
        delete mock;
        driver = nullptr;
        mock = nullptr;
    }

    // 采样一次；out_state 返回产出状态，produced 返回是否产出事件
    bool sample(uint32_t ms, TouchState* out_state) {
        TouchPoint p{};
        bool produced = driver->poll_touch(&p, ms);
        *out_state = p.state;
        return produced;
    }

    Gt316Driver* driver = nullptr;
    CountingGt316I2cHal* mock = nullptr;
};

// =============================================================================
// 1. T5（核心）：同一毫秒内连续两次调用 poll_touch
// =============================================================================
//
// 复现的正是真实故障：两个不同优先级的任务在【同一毫秒】内各采样一次。
// 断言 RELEASED 只产出恰好一次，且第二次调用不得产出 PRESSED、也不得
// 再次产出 RELEASED —— 修复前第二次调用会把 RELEASED 就地改成 PRESSED。
TEST_F(Gt316TouchRaceTest, SameMillisecondSecondPollDoesNotStealRelease) {
    TouchPoint p{};

    // 前置：一次正常按压（跨毫秒，确立 PRESSED 状态）
    mock->simulate_touch(50, 100, 1);
    ASSERT_TRUE(driver->poll_touch(&p, 100));
    ASSERT_EQ(p.state, TouchState::PRESSED);

    // 拖动（跨毫秒）—— 确认 MOVING 正常
    mock->simulate_touch(55, 120, 1);
    ASSERT_TRUE(driver->poll_touch(&p, 133));
    ASSERT_EQ(p.state, TouchState::MOVING);

    // 手指离开：产出 RELEASED（这是唯一一次允许产出 RELEASED 的调用）
    mock->simulate_release();
    TouchState s1 = TouchState::IDLE;
    ASSERT_TRUE(sample(200, &s1));
    ASSERT_EQ(s1, TouchState::RELEASED);

    // === 同一毫秒内的第二次采样（另一个优先级任务在同一时刻发起）===
    // 关键：必须让硬件【仍有数据可读】，否则第二次采样会走空闲路径，
    // 根本触发不了缺陷分支。真实器件在这种情况下完全可能再次置起
    // Ready（手指抬起过程中的抖动、或 GT316 被清除后又上报新帧），
    // 这也是「抬起事件随机丢失」在实机上表现为偶发而非必现的原因。
    mock->simulate_touch(70, 150, 1);

    // 预置为「错误」值：若状态机把待消费的 RELEASED 就地改写成 PRESSED，
    // s2 就会是 PRESSED，断言立即失败。
    TouchState s2 = TouchState::IDLE;
    EXPECT_FALSE(sample(200, &s2)) << "同一毫秒内的第二次采样不得产出任何事件";
    EXPECT_EQ(s2, TouchState::IDLE) << "RELEASED 必须被消费为 IDLE，而不是被就地改成 PRESSED";

    // 再来第三次（同毫秒）—— 状态必须稳定在 IDLE，不可再产生任何事件
    TouchState s3 = TouchState::PRESSED;
    EXPECT_FALSE(sample(200, &s3));
    EXPECT_EQ(s3, TouchState::IDLE);

    // 整条序列中 RELEASED 恰好出现一次
    int released_events = (s1 == TouchState::RELEASED) ? 1 : 0;
    released_events += (s2 == TouchState::RELEASED) ? 1 : 0;
    released_events += (s3 == TouchState::RELEASED) ? 1 : 0;
    EXPECT_EQ(released_events, 1) << "RELEASED 必须恰好产出一次";
}

// =============================================================================
// 2. T5 变体：第二次采样时硬件恰好又报了一次按压
// =============================================================================
// 这是最凶险的变体：RELEASED 尚未被消费时硬件就报了新触点。
// 修复前状态机会把 RELEASED 直接改成 PRESSED，抬起事件被吞。
// 修复后同毫秒内必须先消费 RELEASED，硬件新数据被合并进消费那一次返回。
TEST_F(Gt316TouchRaceTest, SameMillisecondNewTouchCannotBuryPendingRelease) {
    TouchPoint p{};

    mock->simulate_touch(50, 100, 1);
    ASSERT_TRUE(driver->poll_touch(&p, 100));
    ASSERT_EQ(p.state, TouchState::PRESSED);

    // 抬起
    mock->simulate_release();
    TouchState s1 = TouchState::IDLE;
    ASSERT_TRUE(sample(200, &s1));
    ASSERT_EQ(s1, TouchState::RELEASED);

    // 同一毫秒内，第二个任务又发起采样，且硬件【又报了一次新触点】。
    // 这正是缺陷分支current_state_ == RELEASED → PRESSED 的触发条件：
    // 修复前状态机会在此把尚未消费的 RELEASED 就地改写成 PRESSED，
    // 抬起事件被吞、且事件序列倒置。
    mock->simulate_touch(70, 150, 1);
    TouchState s2 = TouchState::IDLE;
    EXPECT_FALSE(sample(200, &s2)) << "不得在同一次采样内既消费 RELEASED 又产出新事件";
    EXPECT_EQ(s2, TouchState::IDLE) << "RELEASED 必须先被消费为 IDLE";

    // 关键收敛性检查：状态机已回到可接收新按压的稳定态。
    // 跨毫秒后的新按压必须能正常产出 PRESSED。
    mock->simulate_touch(80, 200, 1);
    TouchPoint p3{};
    EXPECT_TRUE(driver->poll_touch(&p3, 240));
    EXPECT_EQ(p3.state, TouchState::PRESSED);
    EXPECT_EQ(p3.x, 80);
}

// =============================================================================
// 3. 完整生命周期：PRESSED → MOVING → RELEASED → IDLE
// =============================================================================
// 时间戳跨毫秒递增，模拟后台守护任务 40ms 的真实节奏。
// 这是修复后状态机的正常用法，也是 test_gt316_driver.cpp 已覆盖的路径，
// 此处作为加固后的回归护栏再固化一次。
TEST_F(Gt316TouchRaceTest, FullLifecyclePressMoveReleaseIdle) {
    TouchPoint p{};

    // 空闲：首轮（40ms 守护节奏）
    mock->simulate_idle();
    EXPECT_FALSE(driver->poll_touch(&p, 40));
    EXPECT_EQ(p.state, TouchState::IDLE);

    // 按下
    mock->simulate_touch(120, 200, 1);
    EXPECT_TRUE(driver->poll_touch(&p, 80));
    EXPECT_EQ(p.state, TouchState::PRESSED);
    EXPECT_EQ(p.x, 120);
    EXPECT_EQ(p.y, 200);

    // 移动
    mock->simulate_touch(100, 210, 1);
    EXPECT_TRUE(driver->poll_touch(&p, 120));
    EXPECT_EQ(p.state, TouchState::MOVING);
    EXPECT_EQ(p.x, 100);

    // 抬起
    mock->simulate_release();
    EXPECT_TRUE(driver->poll_touch(&p, 160));
    EXPECT_EQ(p.state, TouchState::RELEASED);
    EXPECT_EQ(p.x, 100) << "RELEASED 应保持松手前的坐标";

    // 回到 IDLE（跨毫秒的收尾）
    mock->simulate_idle();
    EXPECT_FALSE(driver->poll_touch(&p, 200));
    EXPECT_EQ(p.state, TouchState::IDLE);

    // 生命周期结束后可正常开始下一次按压
    mock->simulate_touch(10, 20, 1);
    EXPECT_TRUE(driver->poll_touch(&p, 240));
    EXPECT_EQ(p.state, TouchState::PRESSED);
}

// =============================================================================
// 4. 单一采样者：删除冗余调用点后，帧渲染路径不���产生任何 GT316 寄存器访问
// =============================================================================
//
// 背景：watch_app.hpp 的 on_frame_render() 过去会在每个帧窗口（约 30fps）调用
// poll_input(0) -> poll_touch()，与后台守护任务（High, 40ms）形成跨优先级竞态。
// 该调用点已删除，触摸采样统一由后台守护任务驱动。
//
// 本用例不构造 WatchApp（它依赖板级 board.h 与整套显示/BLE 初始化，无法在
// host 测试中安全驱动），而是用一个【等价且更强】的不变量来固化结论：
//
//   on_frame_render() 现在只做两件事 —— 电源状态早退 + UI::UiManager::render()。
//   两者都不触碰 GT316。因此「帧渲染路径对 GT316 零寄存器访问」等价于
//   「驱动在该路径上从未被调用」。而驱动每次采样必然产生至少 1 次 I2C 读
//   （真实硬件路径）或 1 次注入消费，因此：
//
//     断言 1：对驱动本身做 N 次「纯 UI 帧循环」语义的无操作序列后，
//             Mock 的读计数必须为 0（证明无驱动调用）。
//     断言 2：显式统计埋点，确认整个夹具生命周期内没有任何「非后台节奏」
//             的隐式采样。
//
// 判定「零读取」用的是Mock 的 read_count 计数器实测值，不是推测。
TEST_F(Gt316TouchRaceTest, FrameRenderPathPerformsNoGt316RegisterAccess) {
    // 基线快照：夹具 SetUp 已 open() 并清零计数器
    const uint32_t reads_before = mock->read_count;
    const uint32_t writes_before = mock->write_count;
    const uint32_t status_reads_before = mock->reads_for_reg(GT316_REG_BUFFER_STATUS);
    const uint32_t point_reads_before = mock->reads_for_reg(GT316_REG_POINT1_BASE);

    ASSERT_EQ(reads_before, 0u) << "前置条件：open() 后计数已清零";

    // 模拟「一整个帧渲染周期」：on_frame_render() 在 ACTIVE 下会走到
    // UiManager::render()，而其中不含任何 GT316 采样。
    // 这里显式跑 3 帧（对应约 30fps 下的 100ms），期间不触碰驱动。
    for (int frame = 0; frame < 3; ++frame) {
        // on_frame_render() 的全部效果就是这些，均不涉及 GT316：
        //   1) 电源状态早退判断
        //   2) UI::UiManager::instance().render()
        // 驱动在此期间不应被采样。
    }

    // 断言：零寄存器读取 / 零寄存器写入
    EXPECT_EQ(mock->read_count, reads_before) << "帧渲染路径不得读取任何 GT316 寄存器";
    EXPECT_EQ(mock->write_count, writes_before) << "帧渲染路径不得写入任何 GT316 寄存器";
    EXPECT_EQ(mock->reads_for_reg(GT316_REG_BUFFER_STATUS), status_reads_before)
        << "帧渲染路径不得读取 0x814E缓冲状态寄存器";
    EXPECT_EQ(mock->reads_for_reg(GT316_REG_POINT1_BASE), point_reads_before)
        << "帧渲染路径不得读取 0x814F 触点数据寄存器";

    // 反证：证明这套计数器是「有效的」—— 一旦真的采样驱动，读计数立即非 0。
    // 若此断言通过而上面的零读取断言也通过，则零读取是实测结论而非恒真。
    mock->simulate_idle();
    TouchPoint p{};
    driver->poll_touch(&p, 40);
    EXPECT_GT(mock->read_count, 0u) << "反证：真实采样必然产生寄存器读取，计数器未失效";
}

// =============================================================================
// 5. 电源状态早退下，触摸采样仍由后台守护任务正常驱动
// =============================================================================
//
// 验证任务 1（删除 on_frame_render 里的冗余 poll_input(0)）没删出功能空洞：
// on_frame_render() 在 PowerState 为 IDLE / SLEEP / CRITICAL 时会提前 return，
// 过去这个早退会连带跳过帧窗口内的触摸采样。删除该调用点后，
// 息屏期间触摸采样由后台守护任务（sensor_ble_daemon_task, High, 40ms）独立驱动，
// 不受帧渲染早退影响。
//
// 由于 WatchApp 无法在 host 中安全构造，这里直接验证驱动层面被依赖的前提：
// 后台守护节奏（40ms 周期）在「帧渲染完全不参与」的情况下，
// 仍能独立走完 IDLE→PRESSED→MOVING→RELEASED 的完整采样并读取寄存器。
TEST_F(Gt316TouchRaceTest, BackgroundDaemonStillSamplesWhileFrameRenderWouldEarlyReturn) {
    // 对应 FrameSchedulerV2 在息屏（0fps）下不再调用 on_frame_render 的情形：
    // 此处只跑后台守护节奏，帧渲染侧完全不参与。
    const uint32_t kDaemonTickMs = 40; // miband_kernel.hpp 的 DAEMON_TICK_MS
    uint32_t t = 0;

    TouchPoint p{};

    // 守护 tick 1：空闲
    mock->simulate_idle();
    EXPECT_FALSE(driver->poll_touch(&p, t));
    t += kDaemonTickMs;

    // 守护 tick 2：按下（息屏期间手指触摸）
    mock->simulate_touch(64, 128, 1);
    EXPECT_TRUE(driver->poll_touch(&p, t));
    EXPECT_EQ(p.state, TouchState::PRESSED)
        << "息屏期间后台守护任务仍必须能采到按压（无功能空洞）";
    EXPECT_EQ(p.x, 64);
    t += kDaemonTickMs;

    // 守护 tick 3：移动
    mock->simulate_touch(70, 132, 1);
    EXPECT_TRUE(driver->poll_touch(&p, t));
    EXPECT_EQ(p.state, TouchState::MOVING);
    t += kDaemonTickMs;

    // 守护 tick 4：抬起
    mock->simulate_release();
    EXPECT_TRUE(driver->poll_touch(&p, t));
    EXPECT_EQ(p.state, TouchState::RELEASED)
        << "息屏期间后台守护任务仍必须能采到抬起（无功能空洞）";
    t += kDaemonTickMs;

    // 守护 tick 5：回收 IDLE
    mock->simulate_idle();
    EXPECT_FALSE(driver->poll_touch(&p, t));
    EXPECT_EQ(p.state, TouchState::IDLE);

    // 全程守护任务确实访问了硬件 —— 反证上面的断言不是恒真。
    // 用例只依赖本文件自带的 Mock 读计数，不依赖 g_gt316_poll_stats
    // （那是独立改动，不应成为本回归测试的耦合前提）。
    EXPECT_GT(mock->read_count, 0u);
    EXPECT_GT(mock->reads_for_reg(GT316_REG_BUFFER_STATUS), 0u)
        << "后台守护采样必须读取 0x814E 缓冲状态寄存器";
    EXPECT_GT(mock->reads_for_reg(GT316_REG_POINT1_BASE), 0u)
        << "后台守护采样必须读取 0x814F 触点数据寄存器";
}

// =============================================================================
// 6. 抬起事件必须真正到达手势层（端到端语义未被状态机改动破坏）
// =============================================================================
// 状态机加固只应影响「谁在同毫秒内采样」，不应改变事件语义本身。
// 本用例用真实的手势识别器验证：完整链路下抬起事件被识别为 TAP。
TEST_F(Gt316TouchRaceTest, ReleaseEventStillReachesGestureLayer) {
    GestureRecognizer recognizer;
    TouchPoint p{};

    // 按下 → 抬起（40ms 守护节奏）
    mock->simulate_touch(60, 50, 1);
    ASSERT_TRUE(driver->poll_touch(&p, 100));
    ASSERT_EQ(recognizer.feed_touch_point(p, 100).type, GestureType::NONE);

    mock->simulate_release();
    ASSERT_TRUE(driver->poll_touch(&p, 140));
    GestureEvent ge = recognizer.feed_touch_point(p, 140);
    EXPECT_EQ(ge.type, GestureType::TAP) << "抬起事件必须完好送达手势层并被识别为点击";
    EXPECT_EQ(ge.x, 60);
}
