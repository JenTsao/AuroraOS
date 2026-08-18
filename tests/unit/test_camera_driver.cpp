#include <gtest/gtest.h>

#include "../../drivers/camera/camera_types.hpp"
#include "../../drivers/camera/camera_driver.hpp"
#include "../../drivers/camera/mock_camera_driver.hpp"
#include "../../drivers/camera/ov2640_driver.hpp"
#include "../../kernel/core/device.hpp"
#include "../../vfs/vfs.hpp"

using namespace auroraos::drivers::camera;
using namespace auroraos::kernel;
using namespace auroraos::hal;

// Mock I2C 硬件适配器用于测试 OV2640 寄存器探测与配置
class MockI2cBus : public II2cHal {
public:
    MockI2cBus() {
        memset(regs_bank0, 0, sizeof(regs_bank0));
        memset(regs_bank1, 0, sizeof(regs_bank1));
        current_bank = 0;

        // 设置 OV2640 预设芯片 ID (Bank 1)
        regs_bank1[SENSOR_MIDH] = 0x7F;
        regs_bank1[SENSOR_MIDL] = 0xA2;
        regs_bank1[SENSOR_PIDH] = 0x26;
        regs_bank1[SENSOR_PIDL] = 0x42;
    }

    bool write(uint8_t /*dev_addr*/, const uint8_t* /*data*/, size_t /*len*/) override {
        return true;
    }

    bool write_reg(uint8_t dev_addr, uint8_t reg_addr, const uint8_t* data, size_t len) override {
        if (dev_addr != OV2640_I2C_ADDR || !data || len == 0) return false;

        if (reg_addr == BANK_SEL) {
            current_bank = data[0];
            return true;
        }

        uint8_t* table = (current_bank == BANK_SENSOR) ? regs_bank1 : regs_bank0;
        table[reg_addr] = data[0];
        last_written_reg = reg_addr;
        last_written_val = data[0];
        write_count++;
        return true;
    }

    bool read(uint8_t /*dev_addr*/, uint8_t* /*data*/, size_t /*len*/) override {
        return true;
    }

    bool read_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t* data, size_t len) override {
        if (dev_addr != OV2640_I2C_ADDR || !data || len == 0) return false;

        const uint8_t* table = (current_bank == BANK_SENSOR) ? regs_bank1 : regs_bank0;
        data[0] = table[reg_addr];
        read_count++;
        return true;
    }

    uint8_t regs_bank0[256];
    uint8_t regs_bank1[256];
    uint8_t current_bank;
    uint8_t last_written_reg = 0;
    uint8_t last_written_val = 0;
    uint32_t write_count = 0;
    uint32_t read_count = 0;
};

// =============================================================================
// MockCameraDriver 核心功能测试
// =============================================================================
class MockCameraTest : public ::testing::Test {
protected:
    void SetUp() override {
        Scheduler::instance().init();
        Scheduler::instance().create_task([]() {}, nullptr, 0, TaskPriority::Idle);
        Scheduler::instance().set_started(true);
        Scheduler::instance().schedule();
    }

    void TearDown() override {
        DeviceRegistry::instance().clear();
    }
};

// 1. 测试基础生命周期与打开关闭
TEST_F(MockCameraTest, LifecycleOpenClose) {
    MockCameraDriver cam("video0");
    EXPECT_EQ(cam.open(), 0);
    EXPECT_TRUE(cam.start());
    EXPECT_TRUE(cam.is_capturing());

    EXPECT_TRUE(cam.stop());
    EXPECT_FALSE(cam.is_capturing());
    EXPECT_EQ(cam.close(), 0);
}

// 2. 测试格式与分辨率设置
TEST_F(MockCameraTest, SetFormatAndResolution) {
    MockCameraDriver cam("video0");
    ASSERT_EQ(cam.open(), 0);

    CameraFormatConfig cfg{};
    cfg.width = 320;
    cfg.height = 240;
    cfg.format = PixelFormat::RGB565;
    cfg.fps = 30;

    EXPECT_EQ(cam.ioctl(CAMERA_IOCTL_SET_FMT, &cfg, nullptr), 0);
    EXPECT_EQ(cam.get_width(), 320);
    EXPECT_EQ(cam.get_height(), 240);
    EXPECT_EQ(cam.get_format(), PixelFormat::RGB565);

    CameraFormatConfig get_cfg{};
    EXPECT_EQ(cam.ioctl(CAMERA_IOCTL_GET_FMT, &get_cfg, nullptr), 0);
    EXPECT_EQ(get_cfg.width, 320);
    EXPECT_EQ(get_cfg.height, 240);
    EXPECT_EQ(get_cfg.format, PixelFormat::RGB565);
    EXPECT_EQ(get_cfg.fps, 30);
}

// 3. 测试 SMPTE 彩条生成与单帧读取
TEST_F(MockCameraTest, ColorBarsPatternAndRead) {
    MockCameraDriver cam("video0");
    ASSERT_EQ(cam.open(), 0);
    ASSERT_TRUE(cam.set_format(160, 120, PixelFormat::RGB565));
    cam.set_pattern(MockPattern::ColorBars);
    ASSERT_TRUE(cam.start());

    // 手动推进一帧
    cam.step_frame();

    // 读取帧数据 (使用 vector 避免占用过多调用栈)
    std::vector<uint16_t> buffer(160 * 120, 0);
    int bytes_read = cam.read(reinterpret_cast<char*>(buffer.data()), buffer.size() * sizeof(uint16_t), 0, nullptr);
    EXPECT_EQ(bytes_read, 160 * 120 * 2);

    // 校验第 1 个彩条 (x = 5 应该为纯白 0xFFFF)
    EXPECT_EQ(buffer[10 * 160 + 5], 0xFFFF);

    // 校验第 2 个彩条 (x = 25 应该为纯黄 0xFFE0)
    EXPECT_EQ(buffer[10 * 160 + 25], 0xFFE0);

    // 校验最后一个彩条 (x = 155 应该为纯黑 0x0000)
    EXPECT_EQ(buffer[10 * 160 + 155], 0x0000);
}

// 4. 测试 IOCTL_GET_FRAME 帧结构体获取与统计信息
TEST_F(MockCameraTest, IoctlGetFrameAndStats) {
    MockCameraDriver cam("video0");
    ASSERT_EQ(cam.open(), 0);
    ASSERT_TRUE(cam.set_format(160, 120, PixelFormat::RGB565));
    ASSERT_TRUE(cam.start());

    // 推进 3 帧
    cam.step_frame();
    cam.step_frame();
    cam.step_frame();

    CameraFrame frame{};
    EXPECT_EQ(cam.ioctl(CAMERA_IOCTL_GET_FRAME, &frame, nullptr), 0);
    EXPECT_TRUE(frame.valid);
    EXPECT_EQ(frame.width, 160);
    EXPECT_EQ(frame.height, 120);
    EXPECT_EQ(frame.length, 160 * 120 * 2);
    EXPECT_EQ(frame.sequence, 3);
    EXPECT_NE(frame.buffer, nullptr);

    CameraStats stats{};
    EXPECT_EQ(cam.ioctl(CAMERA_IOCTL_GET_STATS, &stats, nullptr), 0);
    EXPECT_EQ(stats.frames_captured, 3);
    EXPECT_EQ(stats.error_count, 0);
}

// 5. 测试图像调节控制 (Controls)
TEST_F(MockCameraTest, ControlsConfiguration) {
    MockCameraDriver cam("video0");
    ASSERT_EQ(cam.open(), 0);

    CameraControls ctrl{};
    ctrl.brightness = 1;
    ctrl.contrast = -1;
    ctrl.hflip = true;
    ctrl.vflip = false;
    ctrl.effect = SpecialEffect::Sepia;
    ctrl.test_pattern = true;

    EXPECT_EQ(cam.ioctl(CAMERA_IOCTL_SET_CONTROLS, &ctrl, nullptr), 0);

    CameraControls get_ctrl{};
    EXPECT_EQ(cam.ioctl(CAMERA_IOCTL_GET_CONTROLS, &get_ctrl, nullptr), 0);
    EXPECT_EQ(get_ctrl.brightness, 1);
    EXPECT_EQ(get_ctrl.contrast, -1);
    EXPECT_TRUE(get_ctrl.hflip);
    EXPECT_FALSE(get_ctrl.vflip);
    EXPECT_EQ(get_ctrl.effect, SpecialEffect::Sepia);
    EXPECT_TRUE(get_ctrl.test_pattern);
}

// 6. 测试 VFS 注册与挂载交互
TEST_F(MockCameraTest, VfsMountAndAccess) {
    VfsManager::instance().init();
    MockCameraDriver cam("camera0");
    ASSERT_TRUE(DeviceRegistry::instance().register_device(&cam));

    int fd = VfsManager::instance().open("/dev/camera0", 0);
    EXPECT_GE(fd, 0);

    CameraFormatConfig cfg{160, 120, PixelFormat::RGB565, 30};
    EXPECT_EQ(VfsManager::instance().ioctl(fd, CAMERA_IOCTL_SET_FMT, &cfg), 0);
    EXPECT_EQ(VfsManager::instance().ioctl(fd, CAMERA_IOCTL_START_CAPTURE, nullptr), 0);

    cam.step_frame();

    char buf[256];
    int r = VfsManager::instance().read(fd, buf, sizeof(buf));
    EXPECT_GT(r, 0);

    EXPECT_EQ(VfsManager::instance().close(fd), 0);
}

// =============================================================================
// Ov2640Driver 硬件驱动测试 (带 Mock I2C 总线)
// =============================================================================
TEST(Ov2640DriverTest, ProbeAndRegisterInitialization) {
    MockI2cBus i2c;
    Ov2640Driver driver(&i2c, nullptr, "video0");

    // 探测应成功
    EXPECT_EQ(driver.open(), 0);

    // 检查写入的寄存器次数
    EXPECT_GT(i2c.write_count, 10);

    // 格式切换
    EXPECT_TRUE(driver.set_format(320, 240, PixelFormat::RGB565));
    EXPECT_TRUE(driver.set_format(160, 120, PixelFormat::JPEG));

    // 控制参数设置 (水平镜像与负片效果)
    CameraControls ctrl{};
    ctrl.hflip = true;
    ctrl.vflip = true;
    ctrl.effect = SpecialEffect::Negative;
    EXPECT_EQ(driver.ioctl(CAMERA_IOCTL_SET_CONTROLS, &ctrl, nullptr), 0);

    EXPECT_EQ(driver.close(), 0);
}

TEST(Ov2640DriverTest, ProbeFailureOnWrongId) {
    MockI2cBus i2c;
    // 注入错误的芯片 ID
    i2c.regs_bank1[SENSOR_PIDH] = 0x00;

    Ov2640Driver driver(&i2c, nullptr, "video0");
    EXPECT_EQ(driver.open(), -1); // 探测失败
}
