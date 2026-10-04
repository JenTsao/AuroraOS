// =============================================================================
// tests/unit/test_i2c_bus.cpp
//
// 覆盖 I2C 总线级串行化原语（hal/i2c_bus.hpp）。
//
// 背景：Apollo3 IOM 的 FIFO/CMD 是共享寄存器外设。GT316 触控 (0x14) 与
// BHI260AP 加速度计 (0x28) 同在一条物理总线上，ui_render_task (Realtime)
// 与 sensor_ble_daemon_task (High) 会并发发起事务，撕裂表现为偶发 NACK
// 或读到错误数据。
//
// 本文件不实例化 Apollo3I2cHal（它写真实 MMIO 地址，宿主无法访问），而是：
//   - 用 FakeIom 复刻其事务结构（地址写入 → 数据推送 → CMD 触发 → 读出），
//     并能观测「事务中途被他人插入」这一撕裂特征；
//   - 用 FakyI2cClient 复刻 Apollo3I2cHal 的加锁结构（公开方法加锁后只调用
//     不加锁的 _write_impl/_read_impl，read_reg 式组合事务单次持锁）。
// 这样 tear_count()==0 才是可证伪的断言，而非仅验证锁计数。
//
// 注意：宿主 Scheduler 是模拟实现（create_task 不真正执行任务体），
// 因此这里不使用 schedule() 来制造并发，而是用「显式递归进入另一客户端」
// 精确模拟抢占点，从而让撕裂可确定性复现。
//
// 覆盖点对应任务要求：
//   1. 正常路径上锁确实被获取和释放
//   2. 多 return 点 —— RAII 在中途失败的提前 return 上仍正确释放锁
//   3. 同一实例上连续两次操作不丢失（无撕裂）
//   4. 降级路径 —— 持锁者永不释放时等待方走有界降级，不无限自旋
// =============================================================================

#include <gtest/gtest.h>

#include "../../hal/i2c_bus.hpp"
#include "arch_api.hpp"
#include "frame_scheduler_v2.hpp"
#include "mutex.hpp"
#include "task.hpp"
#include "timer.hpp"

// 锁原语位于 auroraos::hal 命名空间，此处引入局部别名简化测试代码
using auroraos::hal::I2cBusGuard;
using auroraos::hal::I2cBusLock;

// 宿主测试推进 tick：与 tests/unit/test_mutex.cpp 同构，让 Mutex 的超时路径
// 能够真正走到 elapsed >= timeout 分支。
static void i2c_bus_test_tick_hook() {
    TimerManager::instance().fast_forward_ticks(1);
    Scheduler::instance().tick_update();
}

class I2cBusTest : public ::testing::Test {
protected:
    void SetUp() override {
        Arch::g_arch_test_interrupt_hook = i2c_bus_test_tick_hook;
        Scheduler::instance().init();
        // 建一个 Idle 任务，保证调度器有回退目标
        Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Idle);
        FrameSchedulerV2::instance().notify_render_complete();
        Scheduler::instance().set_started(true);
    }

    void TearDown() override {
        Arch::g_arch_test_interrupt_hook = nullptr;
    }
};

// =============================================================================
// FakeIom —— 复刻 Apollo3 IOM I2C 寄存器的可观测模型
//
// 真实硬件上 FIFO/CMD 是全局寄存器：两个任务交错写就会互相破坏。这里用
// transaction_active_ 标记「当前是否有事务在飞」，在事务进行中再次进入即
// 记录一次撕裂。
//
// 注意 tear_count 只统计「真正的寄存器撕裂」：某客户端在别的客户端事务中途
// 仍然改了寄存器。合法的总线仲裁（第二个客户端被总线锁挡在门外，直接失败）
// 不算撕裂 —— 否则「锁生效」反而会被误判成「发生撕裂」，把修复效果弄反。
// =============================================================================
class FakeIom {
public:
    static constexpr size_t kMaxPayload = 16;

    // 返回 false 表示检测到撕裂（事务被他人打断）
    bool begin_transaction(uint32_t client_id) {
        if (transaction_active_) {
            ++tear_count_;
            return false;
        }
        transaction_active_ = true;
        owner_ = client_id;
        return true;
    }

    void end_transaction() {
        transaction_active_ = false;
        owner_ = 0xFFFFFFFFu;
    }

    // 模拟一次任务抢占：让另一个客户端在当前事务中途插入。
    // 由测试显式指定「入侵者」，避免依赖宿主调度器真的切换任务。
    void set_interrupter(void (*fn)(void*), void* ctx) {
        interrupter_ = fn;
        interrupter_ctx_ = ctx;
    }

    void maybe_interrupt() {
        if (interrupter_) {
            void (*fn)(void*) = interrupter_;
            void* ctx = interrupter_ctx_;
            interrupter_ = nullptr; // 只打断一次，避免递归
            fn(ctx);
        }
    }

    void record(uint8_t dev_addr, uint16_t reg, const uint8_t* payload, size_t len) {
        last_dev_ = dev_addr;
        last_reg_ = reg;
        last_len_ = len;
        for (size_t i = 0; i < len && i < kMaxPayload; ++i) {
            last_payload_[i] = payload[i];
        }
        ++commit_count_;
    }

    void read_back(uint16_t reg, uint8_t* out, size_t len) {
        for (size_t i = 0; i < len && i < kMaxPayload; ++i) {
            out[i] = static_cast<uint8_t>(last_payload_[i] ^ static_cast<uint8_t>(reg));
        }
    }

    uint32_t tear_count() const { return tear_count_; }
    uint32_t commit_count() const { return commit_count_; }
    uint8_t last_dev() const { return last_dev_; }
    uint16_t last_reg() const { return last_reg_; }

private:
    volatile bool transaction_active_ = false;
    uint32_t owner_ = 0xFFFFFFFFu;
    uint32_t tear_count_ = 0;
    uint32_t commit_count_ = 0;
    uint8_t last_dev_ = 0;
    uint16_t last_reg_ = 0;
    size_t last_len_ = 0;
    uint8_t last_payload_[kMaxPayload] = {0};
    void (*interrupter_)(void*) = nullptr;
    void* interrupter_ctx_ = nullptr;
};

// =============================================================================
// SharedBus — 共享总线的测试替身
//
// kernel::Mutex 是「按 TCB 递归」的（kernel/core/mutex.hpp:200-203：同一 TCB
// 重复 lock() 会走 recursive_count_ 而非阻塞）。而宿主测试是单线程的，两个
// 假客户端其实处于同一个 TCB，直接共用一把 Mutex 无法表达「被另一个任务
// 持有」， intruder 会递归重入而不是被挡住 —— 这会让测试出现依赖执行顺序的
// 假失败（是否重入取决于当时 Scheduler::get_current_tcb() 是否为空，而该值
// 受其它测试套件的残留状态影响）。
//
// 因此这里用一个显式的 owner 标记来表达「总线当前被谁占用」，语义严格对齐
// 真实固件：IOM 只有一个，两个不同任务必须互斥进入。真实固件里这一互斥由
// Mutex 提供；本替身只替换「如何判断被占用」这一不可模拟的部分，其余
// （RAII 作用域、提前 return 释放、降级计数）仍走真实的 I2cBusLock/I2cBusGuard。
// =============================================================================
class SharedBus {
public:
    static constexpr uint32_t kNoOwner = 0xFFFFFFFFu;

    // 返回 false 表示总线已被他人占用（对应固件里 lock() 拿不到锁）
    bool try_acquire(uint32_t client_id) {
        if (owner_ != kNoOwner) {
            ++contended_count_;
            return false;
        }
        owner_ = client_id;
        return true;
    }

    void release(uint32_t client_id) {
        if (owner_ == client_id) {
            owner_ = kNoOwner;
        }
    }

    uint32_t owner() const { return owner_; }
    uint32_t contended_count() const { return contended_count_; }

private:
    uint32_t owner_ = kNoOwner;
    uint32_t contended_count_ = 0;
};

// =============================================================================
// FakyI2cClient —— 复刻 Apollo3I2cHal 的加锁结构
//
// 公开方法：加锁 → 校验 → 调 _impl。
// 私有 _impl：绝不加锁（锁非递归，重入即自死锁）。
// read_combo 复刻 read_reg：单次持锁内先写寄存器地址再读回数据 —— 若错误地
// 调用公开的 write()/read()，非递归锁会自死锁，这正是本次要修的隐患。
// =============================================================================
class FakyI2cClient {
public:
    static constexpr size_t kMaxTransfer = 8;

    FakyI2cClient(FakeIom* iom, uint8_t dev_addr, uint32_t client_id,
                  I2cBusLock* lock, SharedBus* bus)
        : iom_(iom), dev_addr_(dev_addr), client_id_(client_id), lock_(lock), bus_(bus) {}

    I2cBusLock& bus_lock() { return *lock_; }
    SharedBus& shared_bus() { return *bus_; }
    uint32_t id() const { return client_id_; }

    bool write(uint16_t reg, const uint8_t* data, size_t len) {
        I2cBusGuard guard(*lock_);
        if (!guard.acquired())
            return false;
        if (!bus_->try_acquire(client_id_))
            return false; // 总线被他人占用：合法仲裁，不算撕裂
        bool ok = write_impl(reg, data, len);
        bus_->release(client_id_);
        return ok;
    }

    // 复刻 read_reg：单次持锁覆盖「写地址 + 读数据」整个组合事务。
    bool read_combo(uint16_t reg, uint8_t* data, size_t len) {
        I2cBusGuard guard(*lock_);
        if (!guard.acquired())
            return false;
        if (!bus_->try_acquire(client_id_))
            return false;
        bool ok = write_impl(reg, nullptr, 0);
        if (ok)
            ok = read_impl(reg, data, len);
        bus_->release(client_id_);
        return ok;
    }

    // 「事务中途失败并提前 return」场景：长度超限。
    // 验证 RAII 在该提前 return 上仍然释放锁，且不产生事务提交。
    bool write_exceeding_length(uint16_t reg, const uint8_t* data, size_t len) {
        I2cBusGuard guard(*lock_);
        if (!guard.acquired())
            return false;
        if (len > kMaxTransfer)
            return false; // 提前 return：锁必须由 guard 释放
        return write_impl(reg, data, len);
    }

    // 对照组：同样的组合事务，但完全不加锁也不做总线仲裁。
    // 证明 FakeIom 确实能检出撕裂，从而使「加锁后 tear_count()==0」有意义。
    bool read_combo_unlocked(uint16_t reg, uint8_t* data, size_t len) {
        if (!write_impl(reg, nullptr, 0))
            return false;
        return read_impl(reg, data, len);
    }

    // 供 interrupter 回调使用：执行一次「完全无保护」的组合事务。
    // 必须与 read_combo_unlocked 同构（同样不加锁、不做仲裁），
    // 否则对照组里入侵者仍会被挡住，对照就失效了。
    bool run_unprotected_read_combo(uint16_t reg, uint8_t* data, size_t len) {
        return read_combo_unlocked(reg, data, len);
    }

    uint8_t dev_addr() const { return dev_addr_; }

private:
    bool write_impl(uint16_t reg, const uint8_t* data, size_t len) {
        if (!iom_->begin_transaction(client_id_))
            return false;
        iom_->maybe_interrupt(); // 抢占点
        iom_->record(dev_addr_, reg, data, len);
        iom_->end_transaction();
        return true;
    }

    bool read_impl(uint16_t reg, uint8_t* data, size_t len) {
        if (!iom_->begin_transaction(client_id_))
            return false;
        iom_->maybe_interrupt();
        iom_->read_back(reg, data, len);
        iom_->end_transaction();
        return true;
    }

    FakeIom* iom_;
    uint8_t dev_addr_;
    uint32_t client_id_;
    I2cBusLock* lock_;
    SharedBus* bus_;
};

// interrupter 需要的上下文
struct InterruptCtx {
    FakyI2cClient* client;
    uint16_t reg;
    // 入侵者走哪条路径：true=与被测方同构的受保护事务，false=完全无保护事务
    bool use_protected_path = true;
    // 是否真的尝试发起事务（防止测试空转）
    bool attempted = false;
    // 入侵者是否成功改写了寄存器（被总线仲裁挡下时为 false）
    bool succeeded = false;
    // 入侵发生时，总线仲裁的持有者是谁。
    // 若锁正确覆盖了整段事务，这里应是被测客户端（证明锁在事务全程有效）。
    uint32_t observed_bus_owner = SharedBus::kNoOwner;
};

static void interrupter_trampoline(void* p) {
    InterruptCtx* ctx = static_cast<InterruptCtx*>(p);
    uint8_t scratch[4] = {0};
    ctx->attempted = true;
    // 记录入侵瞬间的总线持有者
    ctx->observed_bus_owner = ctx->client->shared_bus().owner();
    // 入侵者尝试在他人事务中途发起自己的事务。
    // 受保护路径下（use_protected_path）总线仲裁会挡住它；无保护路径下
    // 则会真的改写寄存器，把对方事务搅乱。
    ctx->succeeded = ctx->use_protected_path
        ? ctx->client->read_combo(ctx->reg, scratch, sizeof(scratch))
        : ctx->client->run_unprotected_read_combo(ctx->reg, scratch, sizeof(scratch));
}

// =============================================================================
// 1. 正常路径：锁确实被获取和释放
// =============================================================================

TEST_F(I2cBusTest, NormalPathAcquiresAndReleases) {
    I2cBusLock lock;
    ASSERT_EQ(lock.acquire_count(), 0u);
    ASSERT_EQ(lock.degraded_count(), 0u);

    {
        I2cBusGuard guard(lock);
        ASSERT_TRUE(guard.acquired());
        EXPECT_EQ(lock.acquire_count(), 1u) << "持锁期间成功计数应已递增";
        EXPECT_EQ(lock.degraded_count(), 0u);
    }

    // 离开作用域后必须能重新获取 —— 证明析构确实释放了锁
    {
        I2cBusGuard guard(lock);
        EXPECT_TRUE(guard.acquired()) << "析构未释放锁";
    }
    EXPECT_EQ(lock.acquire_count(), 2u);
    EXPECT_EQ(lock.degraded_count(), 0u);
}

// 诊断计数器可复位
TEST_F(I2cBusTest, DiagnosticsCanBeReset) {
    I2cBusLock lock;
    {
        I2cBusGuard guard(lock);
        ASSERT_TRUE(guard.acquired());
    }
    ASSERT_EQ(lock.acquire_count(), 1u);
    lock.reset_diagnostics();
    EXPECT_EQ(lock.acquire_count(), 0u);
    EXPECT_EQ(lock.degraded_count(), 0u);
}

// =============================================================================
// 2. 多 return 点：事务中途失败提前 return 时 RAII 仍正确释放锁
// =============================================================================

TEST_F(I2cBusTest, EarlyReturnInsideTransactionStillReleasesLock) {
    FakeIom iom;
    SharedBus bus;
    I2cBusLock lock;
    FakyI2cClient client(&iom, 0x14, 1, &lock, &bus);

    uint8_t payload[16] = {0};
    // 超过 kMaxTransfer 的长度会在事务中途提前 return
    EXPECT_FALSE(client.write_exceeding_length(0x8040, payload, FakyI2cClient::kMaxTransfer + 1));

    // 关键断言：提前 return 之后锁必须已释放，且未产生任何事务提交
    EXPECT_EQ(lock.degraded_count(), 0u);
    EXPECT_EQ(iom.commit_count(), 0u);
    EXPECT_EQ(iom.tear_count(), 0u);

    {
        I2cBusGuard guard(lock);
        EXPECT_TRUE(guard.acquired()) << "提前 return 路径漏解锁";
    }
}

// 连续多次「进入作用域即提前 return」不应把锁耗尽
TEST_F(I2cBusTest, RepeatedEarlyReturnsDoNotExhaustLock) {
    FakeIom iom;
    SharedBus bus;
    I2cBusLock lock;
    FakyI2cClient client(&iom, 0x14, 1, &lock, &bus);
    uint8_t payload[16] = {0};

    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(client.write_exceeding_length(0x8040, payload,
                                                   FakyI2cClient::kMaxTransfer + 1));
    }

    EXPECT_EQ(lock.degraded_count(), 0u);
    // 5 次提前 return + 1 次正常获取
    EXPECT_EQ(lock.acquire_count(), 5u);

    I2cBusGuard guard(lock);
    EXPECT_TRUE(guard.acquired()) << "多次提前 return 后锁未释放";
}

// 提前 return 与正常事务交替：正常事务必须仍能提交，数据不丢失
TEST_F(I2cBusTest, InterleavedEarlyReturnsAndSuccessesLoseNoData) {
    FakeIom iom;
    SharedBus bus;
    I2cBusLock lock;
    FakyI2cClient client(&iom, 0x14, 1, &lock, &bus);
    uint8_t payload[16] = {0};

    for (int i = 0; i < 8; ++i) {
        // 偶数次走提前 return
        if (i % 2 == 0) {
            EXPECT_FALSE(client.write_exceeding_length(0x8040, payload,
                                                       FakyI2cClient::kMaxTransfer + 1));
        } else {
            EXPECT_TRUE(client.write(0x8040, payload, 4));
        }
    }

    EXPECT_EQ(iom.tear_count(), 0u);
    EXPECT_EQ(iom.commit_count(), 4u) << "成功事务次数不符";
    EXPECT_EQ(lock.degraded_count(), 0u);
}

// =============================================================================
// 3. 同一实例上连续两次操作不丢失（无撕裂）
// =============================================================================

TEST_F(I2cBusTest, ConsecutiveOperationsOnSameInstanceDoNotTear) {
    FakeIom iom;
    SharedBus bus;
    I2cBusLock lock;
    FakyI2cClient touch(&iom, 0x14, 1, &lock, &bus);

    // 第一次操作
    ASSERT_TRUE(touch.write(0x8040, nullptr, 0));
    EXPECT_EQ(iom.last_dev(), 0x14);
    EXPECT_EQ(iom.last_reg(), 0x8040);
    uint32_t commits_after_first = iom.commit_count();

    // 第二次操作 —— 寄存器地址必须完整更新，不能残留第一次的内容
    uint8_t payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    ASSERT_TRUE(touch.write(0x8140, payload, 4));

    EXPECT_EQ(iom.commit_count(), commits_after_first + 1);
    EXPECT_EQ(iom.last_reg(), 0x8140);
    EXPECT_EQ(iom.tear_count(), 0u) << "连续操作之间发生事务撕裂";

    // read_combo 复刻 read_reg：单次持锁内完成写地址+读数据，不自死锁
    uint8_t out[4] = {0};
    ASSERT_TRUE(touch.read_combo(0x8140, out, sizeof(out)));
    EXPECT_EQ(iom.tear_count(), 0u);
}

// 两个设备共用一条物理总线（GT316 0x14 + BHI260AP 0x28，真实场景）：
// 共享同一 I2cBusLock，交替访问 10 轮不得撕裂
TEST_F(I2cBusTest, SharedBusLockSerializesTwoDevices) {
    FakeIom iom;
    SharedBus bus;
    I2cBusLock shared_bus_lock;
    FakyI2cClient touch(&iom, 0x14, 1, &shared_bus_lock, &bus);
    FakyI2cClient accel(&iom, 0x28, 2, &shared_bus_lock, &bus);

    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(touch.write(0x8040, nullptr, 0)) << "第 " << i << " 轮触摸事务失败";
        ASSERT_TRUE(accel.write(0x2C, nullptr, 0)) << "第 " << i << " 轮加速度计事务失败";
    }

    EXPECT_EQ(iom.tear_count(), 0u) << "共享总线上出现事务撕裂";
    EXPECT_EQ(iom.commit_count(), 20u);
    // 两个实例各自独立计数，但共享同一把锁
    EXPECT_EQ(shared_bus_lock.acquire_count(), 20u);
    EXPECT_EQ(shared_bus_lock.degraded_count(), 0u);
}

// 关键：抢占点在事务中途。锁生效时，入侵者 accel 的 read_combo 在
// write_impl 的抢占点被调用，此时总线已被 touch 占用，仲裁必须让 accel
// 直接失败（不改寄存器）—— 这才是「无撕裂」的真正原因。
TEST_F(I2cBusTest, InterruptionDuringTransactionIsPreventedByLock) {
    FakeIom iom;
    SharedBus bus;
    I2cBusLock shared_bus_lock;
    FakyI2cClient touch(&iom, 0x14, 1, &shared_bus_lock, &bus);
    FakyI2cClient accel(&iom, 0x28, 2, &shared_bus_lock, &bus);

    InterruptCtx ctx{&accel, 0x2C};
    iom.set_interrupter(&interrupter_trampoline, &ctx);

    uint8_t out[4] = {0};
    // touch 的事务在 write_impl 内被 accel 打断；因总线已被占用，accel 被挡下
    EXPECT_TRUE(touch.read_combo(0x8040, out, sizeof(out)));

    // 入侵者确实尝试进入了（否则本测试是空转）
    EXPECT_TRUE(ctx.attempted) << "入侵者未被触发，测试失去意义";
    // 核心断言：入侵发生时，总线仍被 touch 持有 —— 说明锁覆盖了整段事务
    // （起始地址写入 + 数据推送 + CMD 触发 + 读出），中途不会被他人插入。
    // 这正是本次修复要保证的性质，且不依赖调度器残留状态。
    EXPECT_EQ(ctx.observed_bus_owner, touch.id())
        << "事务中途总线已被释放，锁未覆盖完整事务";
    // 但被仲裁挡下：从未改写寄存器
    EXPECT_FALSE(ctx.succeeded) << "入侵者在他人事务中途改写了寄存器";
    EXPECT_EQ(iom.tear_count(), 0u) << "抢占点处发生事务撕裂";
    // touch 的事务完整走完（写地址 + 读数据两段都在锁内）
    EXPECT_EQ(iom.commit_count(), 1u);
    // 事务结束后总线已释放，未泄漏占用
    EXPECT_EQ(bus.owner(), SharedBus::kNoOwner) << "事务结束后总线占用泄漏";
}

// 反面对照：证明 FakeIom 真的能检出撕裂，否则上面的 tear_count()==0 无意义
TEST_F(I2cBusTest, UnprotectedTransactionsDoTear_ControlCase) {
    FakeIom iom;
    SharedBus bus;
    I2cBusLock shared_bus_lock;
    FakyI2cClient touch(&iom, 0x14, 1, &shared_bus_lock, &bus);
    FakyI2cClient accel(&iom, 0x28, 2, &shared_bus_lock, &bus);

    InterruptCtx ctx{&accel, 0x2C};
    ctx.use_protected_path = false; // 对照组：入侵者同样不受保护
    iom.set_interrupter(&interrupter_trampoline, &ctx);

    uint8_t out[4] = {0};
    // 不加锁也不仲裁：accel 在 touch 的事务中途强行插入 → 撕裂。
    // 注意 read_combo_unlocked 自身会因 begin_transaction 失败而返回 false
    // （事务中途被打断，本客户端没能提交），这正是「无保护」的表现；
    // 断言的是「入侵确实发生并被检出」，而非入侵者是否得逞。
    touch.read_combo_unlocked(0x8040, out, sizeof(out));

    // 入侵者确实触发了（否则本测试是空转）
    EXPECT_TRUE(ctx.attempted) << "入侵者未被触发，测试失去意义";
    // 对照差异：不受保护时，总线仲裁根本没有参与，入侵瞬间没有持有者。
    // 这与受保护用例中 observed_bus_owner==touch.id() 形成直接对照。
    EXPECT_EQ(ctx.observed_bus_owner, SharedBus::kNoOwner)
        << "对照组不应有总线持有者";
    // FakeIom 检出了撕裂 —— 证明 tear_count()==0 在受保护用例中是有意义的
    EXPECT_GT(iom.tear_count(), 0u) << "FakeIom 无法检出撕裂，测试失去意义";
    // 无仲裁时不会记录 contention（SharedBus 根本没参与）
    EXPECT_EQ(bus.contended_count(), 0u) << "对照组不应发生总线仲裁";
}

// =============================================================================
// 4. 降级路径：持锁者永不释放时等待方走有界降级，不无限自旋
//
// 使用真实 kernel Mutex + 宿主 Scheduler 模拟（与 tests/unit/test_mutex.cpp
// 的 LockTimeout 同构）：holder 拿锁后被挂起，waiter 带超时尝试加锁。
// Mutex::lock(timeout) 以 tick 计时，超时必返回 false —— 环路有界。
// =============================================================================

TEST_F(I2cBusTest, BoundedDegradationWhenHolderNeverReleases) {
    // 1 tick 超时，让降级路径在测试内快速走完
    I2cBusLock lock(/*acquire_timeout_ticks=*/1);

    // holder 拿到锁后永不释放（模拟持锁者卡死）
    TaskControlBlock* holder =
        Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Low);
    Scheduler::instance().schedule();
    {
        I2cBusGuard held(lock);
        ASSERT_TRUE(held.acquired());
        ASSERT_EQ(lock.acquire_count(), 1u);

        // 挂起 holder，使 waiter 成为当前任务
        Scheduler::instance().set_task_state(holder->scheduler.id, TaskState::Suspended);
        TaskControlBlock* waiter =
            Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Realtime);
        Scheduler::instance().schedule();

        uint32_t degraded_before = lock.degraded_count();

        // waiter 走降级路径：必须在有限时间内返回 false，而非无限自旋
        {
            I2cBusGuard guard(lock);
            EXPECT_FALSE(guard.acquired()) << "持锁者永不释放时，等待方不应声称拿到了锁";
            EXPECT_EQ(lock.degraded_count(), degraded_before + 1) << "未记录降级诊断计数";
            EXPECT_EQ(lock.acquire_count(), 1u) << "降级路径不应计入成功获取次数";
        }

        // 清理：切回 holder 后释放锁，避免影响后续用例
        Scheduler::instance().set_task_state(waiter->scheduler.id, TaskState::Suspended);
        Scheduler::instance().set_task_state(holder->scheduler.id, TaskState::Ready);
        Scheduler::instance().schedule();
    }
}

// 降级发生时守卫不得误解他人的锁：等持锁者释放后，等待方能正常获取
TEST_F(I2cBusTest, NoLockLeakAfterDegradation) {
    I2cBusLock lock(/*acquire_timeout_ticks=*/1);

    TaskControlBlock* holder =
        Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Low);
    Scheduler::instance().schedule();

    {
        I2cBusGuard held(lock);
        ASSERT_TRUE(held.acquired());
        Scheduler::instance().set_task_state(holder->scheduler.id, TaskState::Suspended);

        TaskControlBlock* waiter =
            Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Realtime);
        Scheduler::instance().schedule();

        {
            I2cBusGuard guard(lock);
            ASSERT_FALSE(guard.acquired());
        } // 降级路径析构：绝不能调用 unlock（否则会释放 holder 的锁）

        EXPECT_EQ(lock.degraded_count(), 1u);

        Scheduler::instance().set_task_state(waiter->scheduler.id, TaskState::Suspended);
        Scheduler::instance().set_task_state(holder->scheduler.id, TaskState::Ready);
        Scheduler::instance().schedule();
    }

    // 锁已回到可用状态
    Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Normal);
    Scheduler::instance().schedule();
    I2cBusGuard guard(lock);
    EXPECT_TRUE(guard.acquired()) << "降级路径误解了他人持有的锁";
}
