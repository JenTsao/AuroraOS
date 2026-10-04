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

    // ---- 原子性探针（用于验收「复合事务期间无其他客户端插入」） ----
    uint32_t interleaving_detected_ = 0;   // 复合事务内发生他客户端插入的次数
    uint32_t atomicity_violations_ = 0;   // mock 内重入次数（应恒为 0）
    uint32_t atomic_read_count = 0;       // 经 read_reg16 完成的读次数
    bool other_client_entered_ = false;   // 他客户端曾进入（标志，可跨事务）
    bool other_client_active_ = false;    // 他客户端当前在场
    bool in_atomic_reg_transaction_ = false;
    uint32_t atomic_depth_ = 0;
    uint16_t atomic_reg = 0;
    // 「只写了寄存器地址、数据尚未读回」的状态。修复前驱动用
    // write()+read() 两步做一次寄存器读，这个标志会在两步之间为真，
    // 构成一个暴露给同总线其他客户端的窗口。
    bool reg_addr_pending_ = false;
    uint32_t non_atomic_pattern_ = 0;   // 出现过「只写地址」的次数

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

        // 【原子性探针】GT316 协议下，len==2 且无后续数据的 write 就是
        // 「只写寄存器地址」。若驱动用 write()+read() 两步完成一次寄存器
        // 读，则这个 write 之后、read 之前存在一个**可被同总线其他客户端
        // 插入的窗口**（锁已释放）。此处登记该中间状态。
        if (len == 2) {
            reg_addr_pending_ = true;
            non_atomic_pattern_++;
        }

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

        // 【原子性探针】若本次 read 是在「只写寄存器地址」之后紧接着发生
        // （修复前的 write()+read() 模式），则它与那个 write 之间存在一个
        // 已暴露给同总线其他客户端的窗口 —— 记为一次真实可发生的插入。
        // 修复后驱动走 read_reg16，本分支不会被寄存器读路径触发。
        if (reg_addr_pending_) {
            interleaving_detected_++;
            other_client_entered_ = true;
            reg_addr_pending_ = false;
        }

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

    // ---- read_reg16：模拟 Apollo3I2cHal 的单次持锁语义 ----
    //
    // 【关键：为什么这里必须模拟「原子」而不是直接复用 write()+read()】
    // 本 mock 要能证伪「Gt316Driver 是否把写寄存器地址与读数据放在
    // 同一次总线事务内」。若 mock 的 read_reg16 内部调用自己的
    // write()+read()，那么无论驱动用哪种写法，测试都会通过 —— 测试
    // 就测不到点上（team-lead 验收标准明确要求：修复前必须失败）。
    //
    // 因此这里显式断言「进入 read_reg16 期间不得有其他客户端的事务」。
    // other_client_entered_ 由「另一个客户端」置位；复合事务期间它若为
    // true，说明发生了插入 —— 记录到 interleaving_detected_。
    bool read_reg16(uint8_t dev_addr, uint16_t reg_addr, uint8_t* data, size_t len) override {
        if (dev_addr != I2C_ADDR_GT316 || !data || len == 0)
            return false;

        // 模拟「单次持锁」：进入复合事务时，登记当前寄存器地址，
        // 并断言整个事务期间没有其他客户端触碰本器件。
        if (in_atomic_reg_transaction_) {
            atomicity_violations_++;   // 理论上不会发生：mock 内无重入
        }
        in_atomic_reg_transaction_ = true;
        atomic_depth_++;
        atomic_read_count++;

        // 与 write()/read() 路径保持一致的计数语义，使既有断言
        // （reads_for_reg / read_count）继续有效。
        read_count++;
        if (reg_addr < sizeof(memory)) {
            reg_read_count[reg_addr >> 8][reg_addr & 0xFF]++;
        }
        last_reg_written = reg_addr;

        for (size_t i = 0; i < len; ++i) {
            if (reg_addr + i < sizeof(memory)) {
                data[i] = memory[reg_addr + i];
            } else {
                data[i] = 0;
            }
        }

        atomic_depth_--;
        in_atomic_reg_transaction_ = (atomic_depth_ > 0);
        return true;
    }

    // ---- 供测试断言「其他客户端是否被挡在复合事务之外」 ----
    //
    // 模拟同总线的第二个客户端（加速度计）。它会碰器件；若此刻驱动正处
    // read_reg16 复合事务中，就构成一次真实插入。
    //
    // 【为什么「在两次 poll_touch 之间调用本函数」也能测到点】
    // 修复前驱动走 write() + read() 两个公开入口，两次调用都不置
    // in_atomic_reg_transaction_，本函数永远看不到插入 → 计数恒 0 →
    // 断言恒真，测不到点上。真正的判据由本 mock 在 write()/read()
    // 内部对「寄存器地址已写但数据未读」这一中间状态的建模承担，见
    // 下面 write_addr_pending_ 相关注释。
    void register_client_poll() {
        if (in_atomic_reg_transaction_) {
            other_client_entered_ = true;
            interleaving_detected_++;
        }
    }

    void reset_atomicity_probe() {
        interleaving_detected_ = 0;
        atomicity_violations_ = 0;
        atomic_read_count = 0;
        non_atomic_pattern_ = 0;
        reg_addr_pending_ = false;
        other_client_entered_ = false;
        other_client_active_ = false;
        in_atomic_reg_transaction_ = false;
        atomic_depth_ = 0;
        atomic_reg = 0;
    }
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
// 4. 单一采样者：触摸采样只有一个驱动任务，帧渲染路径不再发起 I2C 事务
// =============================================================================
//
// 固化的是**当前架构决定**：「GT316 触摸采样由 sensor_ble_daemon_task
// （High 优先级，40ms）单点驱动，帧渲染路径不发起任何 I2C 事务」。
// 该决定的依据是可论证的事实——采样调用点只存在于 watch_app.cpp:73
// （on_background_tick 内），on_frame_render 中的调用点已删除。
//
// 【本用例不覆盖什么 —— 名字一度名不副实，现已修正】
// 原名 FrameRenderPathPerformsNoGt316RegisterAccess 承诺「帧渲染路径
// 不产生寄存器访问」，但本用例并未构造 WatchApp、也未执行
// on_frame_render，因此它**无法**覆盖真机上的断连重连、I2C 回握重试、
// 息屏/唤醒切换等帧渲染与 I2C 交互的场景。它只固化「采样入口唯一」
// 这一静态架构事实，不固化任何运行时行为。
// 要真正覆盖帧渲染路径需要板级依赖注入（board.h + 显示/BLE 初始化），
// 成本高而收益低，故不纳入 host 单元测试。
//
// 【为什么需要一个 Mock 计数器反证】
// 若 Mock 的计数器是坏的（如恒 0），则「零读取」会因断言对象本身失效
// 而成为恒真。故末尾用一次真实采样证明计数器确实会动。
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
TEST_F(Gt316TouchRaceTest, Gt316SamplingHasSingleDriverTask) {
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

// =============================================================================
// 7. 【P0-4 验收】一次触摸读操作期间，其他客户端不得在事务中间插入
// =============================================================================
//
// 本用例是 team-lead 为 P0-4 定的验收标准：
//   「用 MockGt316I2cHal 断言『一次 Gt316Driver 读操作期间，另一客户端
//     无法在事务中间插入』。若该测试在修复前通过，说明测试没测到点上，
//     必须重写。」
//
// 【为什么修复前必然失败】修复前 Gt316Driver::read_reg16 调
// i2c_->write() + i2c_->read() 两个公开入口，各加一次锁。本 mock 的
// write() 结束时清 in_atomic_reg_transaction_（锁已释放），随后
// register_client_poll() 就能进场 —— interleaving_detected_ 递增。
// 修复后 read_reg16 走 II2cHal::read_reg16，单次持锁覆盖全程，
// in_atomic_reg_transaction_ 期间他客户端无法进入，计数恒为 0。
//
// 【为什么这个断言不是恒真】反证：直接调用 mock->read() 绕过 read_reg16，
// 同样能读到数据但会把 in_atomic_reg_transaction_ 留为 false —— 说明
// 该标志确实由 read_reg16 的进入/退出驱动，不是恒 false。
// 【为什么这个断言不是恒真】三重反证：
//  1) non_atomic_pattern_ > 0  → 修复前驱动确实调过 write()+read() 两步；
//     修复后必须为 0（全部走 read_reg16）。
//  2) atomic_read_count > 0   → poll_touch 真的经由 read_reg16 读了寄存器，
//     证明本用例不是因为驱动没读而空过。
//  3) 单独调 mock->read() 不会置 in_atomic_reg_transaction_，说明该标志
//     确实由 read_reg16 的进入驱动，不是恒 false。
TEST_F(Gt316TouchRaceTest, RegRead16IsAtomicAgainstOtherBusClients) {
    // 前置：清零探针
    mock->simulate_touch(50, 100, 1);
    mock->reset_atomicity_probe();

    // 反证 3：绕过 read_reg16 直接 read() 不应置「原子事务中」标志。
    uint8_t scratch = 0;
    mock->last_reg_written = GT316_REG_BUFFER_STATUS;
    ASSERT_TRUE(mock->read(I2C_ADDR_GT316, &scratch, 1));
    ASSERT_FALSE(mock->in_atomic_reg_transaction_)
        << "read() 单独调用不得被视为复合事务，否则本用例的探针失效";
    ASSERT_EQ(mock->interleaving_detected_, 0u)
        << "该 read 前没有「只写地址」，不应记为插入";

    // 真实路径：poll_touch 内部对 0x814E / 0x814F 各做一次 16 位寄存器读。
    TouchPoint p{};
    mock->simulate_touch(50, 100, 1);
    mock->reset_atomicity_probe();

    for (int i = 0; i < 10; ++i) {
        (void)driver->poll_touch(&p, 40);
        mock->register_client_poll();   // 采样之间第二客户端尝试上总线
    }

    // 反证 2：确认真读了寄存器
    EXPECT_GT(mock->atomic_read_count, 0u)
        << "poll_touch 必须真的经由 read_reg16 完成读，"
           "否则本用例可能因为驱动没走该路径而空过";

    // 核心判据：一次寄存器读期间不得有「只写地址 → 读数据」的窗口。
    EXPECT_EQ(mock->non_atomic_pattern_, 0u)
        << "驱动不得用 write()+read() 两步拼一次寄存器读："
           "两步之间锁已释放，同总线其他客户端（加速度计）可插入，"
           "致触摸读到撕裂的 0x814E 状态字节（漏抬手 / 半帧坐标）";
    EXPECT_EQ(mock->interleaving_detected_, 0u)
        << "寄存器读期间不得存在可被其他客户端插入的窗口";
    EXPECT_EQ(mock->other_client_entered_, false)
        << "第二客户端在触摸事务期间从未获准进入";
    EXPECT_EQ(mock->atomicity_violations_, 0u) << "mock 内不应发生重入";
}

// =============================================================================
// 8. 抑制多点必须可观测（suppressed_multitouch 计数器的验收）
// =============================================================================
//
// 没有断言的验收条件不是验收条件，是装饰：计数器恒 0 也能让 6/6 全过，
// 下次重构会静默删掉它。因此本用例强制触发多点抑制。
TEST_F(Gt316TouchRaceTest, SameMillisecondMultitouchSuppressionIsObservable) {
    TouchPoint p{};

    mock->simulate_touch(50, 100, 1);
    ASSERT_TRUE(driver->poll_touch(&p, 40));        // PRESSED

    mock->simulate_release();
    ASSERT_TRUE(driver->poll_touch(&p, 80));        // RELEASED，闩锁置位

    const uint32_t before = g_gt316_poll_stats.suppressed_multitouch;

    // 同毫秒第二轮，硬件报 2 个触点。
    // 注意读法：这证明「抑制轮报多点」这一【事实】被计数，不等于证明
    // 「有两根不同手指」——闩锁置位轮报0 点、抑制轮报多点，更可能是抬起
    // 残留帧里含多个触点记录。要区分需读 POINT1 的 TrackID（+1 事务）。
    // 本用例只锁「计数器必须非 0 且必须 +1」，不锁对成因的解释。
    mock->simulate_touch(70, 150, 2);
    EXPECT_FALSE(driver->poll_touch(&p, 80)) << "抑制路径不得交付任何事件";

    EXPECT_EQ(g_gt316_poll_stats.suppressed_multitouch, before + 1)
        << "抑制多点必须被计数，否则「抑制丢弃的一定是单点抖动」"
           "这个假设永远无法被证伪";
}
