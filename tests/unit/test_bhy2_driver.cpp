// =============================================================================
// tests/unit/test_bhy2_driver.cpp
//
// Bosch BHI260AP (BHY2 协议) 主机接口层测试：
//   - FIFO 字节流解析器：帧提取 / 系统事件跳过 / 跨块拼接 / 失步与填充安全
//   - 主机接口：探测、CONFIG_SENSOR 命令包字节、read_accel 端到端
//   - AccelerometerSensor 集成：失败语义（无硬件不造假数据）与真实路径计步
// =============================================================================
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

#include "../../hal/i2c_hal.hpp"
#include "../../drivers/sensor/bhy2_driver.hpp"
#include "../../drivers/sensor/sensor_framework.hpp"

using namespace auroraos::bhy2;

// =============================================================================
// 模拟 I2C HAL：8 位寄存器空间 + 顺序读模型（BHI260AP 真实交互方式）
// =============================================================================
class MockBhy2I2cHal : public auroraos::hal::II2cHal {
public:
    uint8_t regs[256];
    bool fail_transfers = false;
    std::vector<uint8_t> cmd_bytes; // 写入 CHAN_CMD(0x00) 的字节流（命令包断言用）

    MockBhy2I2cHal() {
        memset(regs, 0, sizeof(regs));
        // 默认芯片在位且固件就绪
        regs[0x2B] = 0x3B;  // CHIP_ID（家族值待 HIL 记录；非 0x00/0xFF 即在位）
        regs[0x25] = 0x30;  // BOOT_STATUS: HOST_INTERFACE_READY | FW_VERIFY_DONE
        regs[0x1C] = 0x89;  // PRODUCT_ID
    }

    // 预置 FIFO 读出内容：从 CHAN_FIFO_W(0x01) 顺序读出 len 字节
    void load_fifo(const uint8_t* data, size_t len) {
        memcpy(&regs[0x01], data, len);
    }

    bool write(uint8_t, const uint8_t* data, size_t len) override {
        if (fail_transfers || !data || len == 0) {
            return false;
        }
        uint8_t reg = data[0];
        for (size_t i = 1; i < len; ++i) {
            regs[static_cast<uint8_t>(reg + i - 1)] = data[i];
        }
        return true;
    }

    bool write_reg(uint8_t, uint8_t reg, const uint8_t* data, size_t len) override {
        if (fail_transfers || (!data && len > 0)) {
            return false;
        }
        // CHAN_CMD(0x00) 是通道 FIFO：一次突发写入的所有字节都进入命令通道
        //（与普通寄存器自增寻址不同），全部记录供命令包断言
        if (reg == 0x00) {
            for (size_t i = 0; i < len; ++i) {
                cmd_bytes.push_back(data[i]);
            }
        }
        for (size_t i = 0; i < len; ++i) {
            regs[static_cast<uint8_t>(reg + i)] = data[i];
        }
        return true;
    }

    bool read(uint8_t, uint8_t*, size_t) override {
        // 随机读统一走 read_reg 语义，此接口在 BHY2 访问模式下不单独使用
        return !fail_transfers;
    }

    bool read_reg(uint8_t, uint8_t reg, uint8_t* data, size_t len) override {
        if (fail_transfers || !data) {
            return false;
        }
        for (size_t i = 0; i < len; ++i) {
            data[i] = regs[static_cast<uint8_t>(reg + i)];
        }
        return true;
    }
};

// 构造 accel passthrough 数据帧：头字节 (sensor_id 1 << 1) + 3 x int16 LE
static size_t put_accel_frame(uint8_t* buf, size_t off, int16_t x, int16_t y, int16_t z) {
    buf[off + 0] = 0x02;
    buf[off + 1] = static_cast<uint8_t>(x & 0xFF);
    buf[off + 2] = static_cast<uint8_t>((x >> 8) & 0xFF);
    buf[off + 3] = static_cast<uint8_t>(y & 0xFF);
    buf[off + 4] = static_cast<uint8_t>((y >> 8) & 0xFF);
    buf[off + 5] = static_cast<uint8_t>(z & 0xFF);
    buf[off + 6] = static_cast<uint8_t>((z >> 8) & 0xFF);
    return off + 7;
}

// =============================================================================
// FIFO 流解析器
// =============================================================================
TEST(Bhy2FifoParserTest, ExtractsAccelFrame) {
    Bhy2FifoParser p;
    uint8_t buf[16] = {0};
    put_accel_frame(buf, 0, -1234, 2345, 1000);

    int16_t xyz[3] = {0, 0, 0};
    EXPECT_EQ(p.feed(buf, 7, xyz), Bhy2FifoParser::Result::kGotAccel);
    EXPECT_EQ(xyz[0], -1234);
    EXPECT_EQ(xyz[1], 2345);
    EXPECT_EQ(xyz[2], 1000);
}

TEST(Bhy2FifoParserTest, SkipsSystemEventsAndDatalessFrames) {
    Bhy2FifoParser p;
    uint8_t buf[32] = {0};
    size_t off = 0;
    buf[off++] = 0xF5; // Timestamp small delta（帧总大小 2，含 ID）
    buf[off++] = 0x10;
    buf[off++] = 0x03; // sensor 1 data-less 通知（无载荷）
    off = put_accel_frame(buf, off, 100, 200, 300);
    buf[off++] = 0xF9; // 填充帧（表值 0，仅 ID 字节）

    int16_t xyz[3] = {0, 0, 0};
    EXPECT_EQ(p.feed(buf, off, xyz), Bhy2FifoParser::Result::kGotAccel);
    EXPECT_EQ(xyz[0], 100);
    EXPECT_EQ(xyz[1], 200);
    EXPECT_EQ(xyz[2], 300);
}

TEST(Bhy2FifoParserTest, LastFrameWins) {
    Bhy2FifoParser p;
    uint8_t buf[32] = {0};
    size_t off = put_accel_frame(buf, 0, 1, 2, 3);
    off = put_accel_frame(buf, off, 900, 0, 1000);

    int16_t xyz[3] = {0, 0, 0};
    EXPECT_EQ(p.feed(buf, off, xyz), Bhy2FifoParser::Result::kGotAccel);
    EXPECT_EQ(xyz[0], 900); // 最后一帧 = 最新样本
    EXPECT_EQ(xyz[2], 1000);
}

TEST(Bhy2FifoParserTest, FrameSplitAcrossFeeds) {
    Bhy2FifoParser p;
    const uint8_t part1[3] = {0x02, 0x2E, 0xFB}; // 帧头 + x 低字节 + x 高字节 (-1234 = 0xFB2E)
    const uint8_t part2[4] = {0x2A, 0x11, 0xE8, 0x03}; // y=0x112A=4394, z=0x03E8=1000

    int16_t xyz[3] = {0, 0, 0};
    EXPECT_EQ(p.feed(part1, sizeof(part1), xyz), Bhy2FifoParser::Result::kNoFrame);
    EXPECT_EQ(p.feed(part2, sizeof(part2), xyz), Bhy2FifoParser::Result::kGotAccel);
    EXPECT_EQ(xyz[0], -1234);
    EXPECT_EQ(xyz[1], 4394);
    EXPECT_EQ(xyz[2], 1000);
}

TEST(Bhy2FifoParserTest, ZeroByteTerminatesStreamCleanly) {
    Bhy2FifoParser p;
    uint8_t buf[16] = {0};
    put_accel_frame(buf, 0, 10, 20, 30);
    // 其余为 0x00（芯片对超量 FIFO 读取的回填），应在 accel 帧后干净终止

    int16_t xyz[3] = {0, 0, 0};
    EXPECT_EQ(p.feed(buf, sizeof(buf), xyz), Bhy2FifoParser::Result::kGotAccel);
    EXPECT_EQ(xyz[2], 30);
}

TEST(Bhy2FifoParserTest, UnknownSensorFrameReportsDesync) {
    Bhy2FifoParser p;
    uint8_t buf[8] = {0x04, 1, 2, 3, 4, 5, 6}; // sensor_id 2 数据帧（未使能）
    int16_t xyz[3] = {0, 0, 0};
    EXPECT_EQ(p.feed(buf, sizeof(buf), xyz), Bhy2FifoParser::Result::kDesync);
}

TEST(Bhy2FifoParserTest, IncompleteCarryFollowedByGarbageReportsDesync) {
    Bhy2FifoParser p;
    uint8_t buf[32];
    memset(buf, 0x10, sizeof(buf));

    int16_t xyz[3] = {0, 0, 0};
    buf[0] = 0x02; // accel 帧头，只给 3 字节 → 不完整帧进遗留缓冲
    buf[1] = 0xAA;
    buf[2] = 0xBB;
    EXPECT_EQ(p.feed(buf, 3, xyz), Bhy2FifoParser::Result::kNoFrame);

    // 拼接后 accel 帧头之后紧跟未知传感器帧（0x10 = sensor 8 数据帧）：
    // 只使能了 accel，出现其它传感器帧必须上报失步，绝不静默吞掉
    EXPECT_EQ(p.feed(buf, 30, xyz), Bhy2FifoParser::Result::kDesync);
}

// =============================================================================
// 主机接口（探测 / 命令包 / FIFO 读取）
// =============================================================================
TEST(Bhy2HostInterfaceTest, ProbeSucceedsWhenReady) {
    MockBhy2I2cHal i2c;
    Bhy2HostInterface bhy2;
    bhy2.configure(&i2c);
    EXPECT_TRUE(bhy2.probe());
}

TEST(Bhy2HostInterfaceTest, ProbeFailsOnBusError) {
    MockBhy2I2cHal i2c;
    i2c.fail_transfers = true;
    Bhy2HostInterface bhy2;
    bhy2.configure(&i2c);
    EXPECT_FALSE(bhy2.probe());
}

TEST(Bhy2HostInterfaceTest, ProbeFailsOnAllOneChipId) {
    MockBhy2I2cHal i2c;
    i2c.regs[0x2B] = 0xFF; // 总线无应答上拉读数
    Bhy2HostInterface bhy2;
    bhy2.configure(&i2c);
    EXPECT_FALSE(bhy2.probe());
}

TEST(Bhy2HostInterfaceTest, ProbeFailsWithoutHostInterfaceReady) {
    MockBhy2I2cHal i2c;
    i2c.regs[0x25] = 0x00; // 固件未就绪（hub 等待固件上传）
    Bhy2HostInterface bhy2;
    bhy2.configure(&i2c);
    EXPECT_FALSE(bhy2.probe());
}

TEST(Bhy2HostInterfaceTest, ProbeFailsOnFwVerifyError) {
    MockBhy2I2cHal i2c;
    i2c.regs[0x25] = 0x10 | 0x40; // READY 但固件校验失败
    Bhy2HostInterface bhy2;
    bhy2.configure(&i2c);
    EXPECT_FALSE(bhy2.probe());
}

TEST(Bhy2HostInterfaceTest, EnableAccelSendsConfigSensorCommand) {
    MockBhy2I2cHal i2c;
    Bhy2HostInterface bhy2;
    bhy2.configure(&i2c);

    ASSERT_TRUE(bhy2.enable_accel(25.0f, 0));

    // 命令包 = 4 字节头 + 9 字节载荷，全部写入 CHAN_CMD
    ASSERT_EQ(i2c.cmd_bytes.size(), 13u);
    // 头：cmd=0x000D LE，payload_len=9
    EXPECT_EQ(i2c.cmd_bytes[0], 0x0D);
    EXPECT_EQ(i2c.cmd_bytes[1], 0x00);
    EXPECT_EQ(i2c.cmd_bytes[2], 0x09);
    EXPECT_EQ(i2c.cmd_bytes[3], 0x00);
    // 载荷：sensor_id=1（accel passthrough）
    EXPECT_EQ(i2c.cmd_bytes[4], 0x01);
    // 载荷：sample_rate = 25.0f IEEE754 LE (0x41C80000)
    EXPECT_EQ(i2c.cmd_bytes[5], 0x00);
    EXPECT_EQ(i2c.cmd_bytes[6], 0x00);
    EXPECT_EQ(i2c.cmd_bytes[7], 0xC8);
    EXPECT_EQ(i2c.cmd_bytes[8], 0x41);
    // 载荷：latency = 0
    EXPECT_EQ(i2c.cmd_bytes[9], 0x00);
    EXPECT_EQ(i2c.cmd_bytes[10], 0x00);
    EXPECT_EQ(i2c.cmd_bytes[11], 0x00);
    EXPECT_EQ(i2c.cmd_bytes[12], 0x00);
}

TEST(Bhy2HostInterfaceTest, ReadAccelEndToEnd) {
    MockBhy2I2cHal i2c;
    Bhy2HostInterface bhy2;
    bhy2.configure(&i2c);
    ASSERT_TRUE(bhy2.probe());
    ASSERT_TRUE(bhy2.enable_accel(25.0f, 0));

    // FIFO 块：时间戳增量 + accel 帧 + 0x00 填充
    // 注意：load_fifo 写 regs[0x01..0x40]，会覆盖同区间的 INT_STATUS(0x2D)，
    // 因此 data-ready 标志必须在 load_fifo 之后设置
    uint8_t fifo[64];
    memset(fifo, 0, sizeof(fifo));
    size_t off = 0;
    fifo[off++] = 0xF5;
    fifo[off++] = 0x42;
    off = put_accel_frame(fifo, off, -100, 50, 1010);
    i2c.load_fifo(fifo, sizeof(fifo));
    i2c.regs[0x2D] = 0x02;

    int16_t xyz[3] = {0, 0, 0};
    EXPECT_TRUE(bhy2.read_accel(xyz));
    EXPECT_EQ(xyz[0], -100);
    EXPECT_EQ(xyz[1], 50);
    EXPECT_EQ(xyz[2], 1010);
}

TEST(Bhy2HostInterfaceTest, ReadAccelReturnsFalseWhenNoDataReady) {
    MockBhy2I2cHal i2c;
    Bhy2HostInterface bhy2;
    bhy2.configure(&i2c);
    ASSERT_TRUE(bhy2.probe());
    ASSERT_TRUE(bhy2.enable_accel(25.0f, 0));

    i2c.regs[0x2D] = 0x00; // 无新数据（正常轮询空转）

    int16_t xyz[3] = {0, 0, 0};
    EXPECT_FALSE(bhy2.read_accel(xyz));
}

TEST(Bhy2HostInterfaceTest, ReadAccelFalseWithoutConfigure) {
    Bhy2HostInterface bhy2;
    int16_t xyz[3] = {0, 0, 0};
    EXPECT_FALSE(bhy2.read_accel(xyz));
    EXPECT_FALSE(bhy2.probe());
}

// =============================================================================
// AccelerometerSensor 集成：失败语义 + 真实路径计步
// =============================================================================
TEST(Bhy2AccelIntegration, ReadFailsWithoutHardwareOrMock) {
    // 关键回归：既无硬件也无 mock 注入时，read() 必须返回 false，
    // 不再兜底虚构的静止 1g (az=1000mg) 数据
    AccelerometerSensor accel;
    accel.power_up();

    SensorData data;
    EXPECT_FALSE(accel.read(&data));
}

TEST(Bhy2AccelIntegration, InitDegradesGracefullyWhenChipAbsent) {
    MockBhy2I2cHal i2c;
    i2c.fail_transfers = true; // 总线无器件

    AccelerometerSensor accel;
    accel.configure(&i2c);
    accel.power_up();

    EXPECT_TRUE(accel.init()); // 传感器缺失不阻塞系统启动

    SensorData data;
    EXPECT_FALSE(accel.read(&data)); // 但也绝不提供编造数据
}

TEST(Bhy2AccelIntegration, RealI2cPathDrivesStepStateMachine) {
    MockBhy2I2cHal i2c;
    AccelerometerSensor accel;
    accel.configure(&i2c);
    accel.power_up();
    ASSERT_TRUE(accel.init()); // probe + enable 成功 → hw_ready_

    SensorData data;
    const uint32_t initial_steps = accel.get_steps();

    // 通过真实 FIFO 路径注入完整步态周期：STABLE → RISING → FALLING → STABLE
    const int16_t sequence[4] = {1000, 1300, 700, 1000};
    for (int i = 0; i < 4; ++i) {
        uint8_t fifo[64];
        memset(fifo, 0, sizeof(fifo));
        put_accel_frame(fifo, 0, 0, 0, sequence[i]);
        i2c.load_fifo(fifo, sizeof(fifo));
        i2c.regs[0x2D] = 0x02; // data ready

        ASSERT_TRUE(accel.read(&data));
        EXPECT_EQ(data.type, SensorType::ACCELEROMETER);
        EXPECT_EQ(data.payload.accel.z, sequence[i]);
    }

    EXPECT_EQ(accel.get_steps(), initial_steps + 1);
}

TEST(Bhy2AccelIntegration, MockInjectionStillWorks) {
    // 宿主既有测试（test_power / test_sensor_framework）依赖 mock 注入路径
    MockBhy2I2cHal i2c;
    AccelerometerSensor accel;
    accel.configure(&i2c);
    accel.power_up();
    accel.init();

    accel.set_mock_data(0, 0, 1300);

    SensorData data;
    EXPECT_TRUE(accel.read(&data)); // mock 优先于失败的真实路径
    EXPECT_EQ(data.payload.accel.z, 1300);
}
