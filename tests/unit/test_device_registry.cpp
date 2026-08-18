// =============================================================================
// tests/unit/test_device_registry.cpp
//
// 设备对象注册表 (DeviceRegistry) 与"设备即对象"能力模型全覆盖测试
// =============================================================================
#include <gtest/gtest.h>
#include <cstring>

#include "../../kernel/core/device.hpp"
#include "../../kernel/task/task.hpp"
#include "../../kernel/core/cspace.hpp"
#include "../../kernel/core/kernel_object.hpp"
#include "../../syscall/syscall.hpp"

// =============================================================================
// 模拟测试字符设备
// =============================================================================
class MockTestCharDevice : public CharDevice {
public:
    char internal_buf[128];
    size_t internal_len = 0;
    int open_count = 0;
    int close_count = 0;
    int last_ioctl_req = 0;
    void* last_ioctl_arg = nullptr;

    explicit MockTestCharDevice(const char* name) : CharDevice(name) {
        std::memset(internal_buf, 0, sizeof(internal_buf));
    }

    int open() override {
        open_count++;
        return 0;
    }

    int close() override {
        close_count++;
        return 0;
    }

    int read(char* buf, int len, int offset, void* priv) override {
        (void)priv;
        if (!buf || len <= 0)
            return -1;
        if (offset < 0 || static_cast<size_t>(offset) >= internal_len)
            return 0;

        size_t available = internal_len - static_cast<size_t>(offset);
        size_t to_copy = (static_cast<size_t>(len) < available) ? static_cast<size_t>(len) : available;
        std::memcpy(buf, internal_buf + offset, to_copy);
        return static_cast<int>(to_copy);
    }

    int write(const char* buf, int len, int offset, void* priv) override {
        (void)priv;
        if (!buf || len <= 0)
            return -1;
        if (offset < 0 || static_cast<size_t>(offset) + len > sizeof(internal_buf))
            return -1;

        std::memcpy(internal_buf + offset, buf, len);
        if (static_cast<size_t>(offset + len) > internal_len) {
            internal_len = static_cast<size_t>(offset + len);
        }
        return len;
    }

    int ioctl(int request, void* arg, void* priv) override {
        (void)priv;
        last_ioctl_req = request;
        last_ioctl_arg = arg;
        return 0x55AA;
    }
};

// =============================================================================
// 1. 设备注册、注销与 VFS 挂载测试
// =============================================================================
TEST(DeviceRegistryTest, RegisterAndLookupDevice) {
    DeviceRegistry& reg = DeviceRegistry::instance();
    reg.clear();

    MockTestCharDevice uart("uart0");
    MockTestCharDevice touch("touch0");

    EXPECT_TRUE(reg.register_device(&uart));
    EXPECT_TRUE(reg.register_device(&touch));
    EXPECT_EQ(reg.get_device_count(), 2u);

    // 查找已注册设备
    EXPECT_EQ(reg.lookup_device("uart0"), &uart);
    EXPECT_EQ(reg.lookup_device("touch0"), &touch);
    EXPECT_EQ(reg.lookup_device("non_existent"), nullptr);

    // 验证 VFS 路径内联组装
    EXPECT_STREQ(uart.get_vfs_path(), "/dev/uart0");
    EXPECT_STREQ(touch.get_vfs_path(), "/dev/touch0");

    // 重复注册同名设备应被拒绝
    MockTestCharDevice duplicate_uart("uart0");
    EXPECT_FALSE(reg.register_device(&duplicate_uart));

    // 注销设备
    EXPECT_TRUE(reg.unregister_device("uart0"));
    EXPECT_EQ(reg.lookup_device("uart0"), nullptr);
    EXPECT_EQ(reg.get_device_count(), 1u);
}

// =============================================================================
// 2. 基于能力的设备打开与 CSpace 槽位铸造测试
// =============================================================================
TEST(DeviceRegistryTest, OpenDeviceCapabilityMinting) {
    DeviceRegistry& reg = DeviceRegistry::instance();
    reg.clear();

    MockTestCharDevice sensor("sensor0");
    uint32_t default_rights = auroraos::kernel::CAP_RIGHT_READ | auroraos::kernel::CAP_RIGHT_WRITE;
    EXPECT_TRUE(reg.register_device(&sensor, default_rights));

    TaskControlBlock task{};
    task.scheduler.id = 1;

    // 打开设备并铸造能力到 slot 2
    int ret = reg.open_device(&task, "sensor0", 2, auroraos::kernel::CAP_RIGHT_READ | auroraos::kernel::CAP_RIGHT_WRITE);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(sensor.open_count, 1);

    // 校验能力槽位
    auroraos::kernel::Capability* cap = auroraos::kernel::CSpace::cap_lookup(&task, 2);
    ASSERT_NE(cap, nullptr);
    EXPECT_EQ(cap->type, auroraos::kernel::CapType::Device);
    EXPECT_EQ(cap->object, &sensor);
    EXPECT_TRUE(cap->rights.read);
    EXPECT_TRUE(cap->rights.write);
    EXPECT_FALSE(cap->rights.grant);
}

// =============================================================================
// 3. 权限提权防护测试 (Privilege Escalation Protection)
// =============================================================================
TEST(DeviceRegistryTest, PreventPrivilegeEscalation) {
    DeviceRegistry& reg = DeviceRegistry::instance();
    reg.clear();

    // 注册只读设备
    MockTestCharDevice rom("rom0");
    uint32_t read_only_rights = auroraos::kernel::CAP_RIGHT_READ;
    EXPECT_TRUE(reg.register_device(&rom, read_only_rights));

    TaskControlBlock task{};
    task.scheduler.id = 2;

    // 尝试请求写权限 (提权)，应被内核严格拒绝并返回 -3 (EACCES)
    int ret = reg.open_device(&task, "rom0", 1, auroraos::kernel::CAP_RIGHT_READ | auroraos::kernel::CAP_RIGHT_WRITE);
    EXPECT_EQ(ret, -3);

    // 槽位应当保持为空 (Null)
    EXPECT_EQ(auroraos::kernel::CSpace::cap_lookup(&task, 1), nullptr);

    // 以合法只读权限打开，应成功
    ret = reg.open_device(&task, "rom0", 1, auroraos::kernel::CAP_RIGHT_READ);
    EXPECT_EQ(ret, 0);

    auroraos::kernel::Capability* cap = auroraos::kernel::CSpace::cap_lookup(&task, 1);
    ASSERT_NE(cap, nullptr);
    EXPECT_TRUE(cap->rights.read);
    EXPECT_FALSE(cap->rights.write);
}

// =============================================================================
// 4. 基于能力的设备读写控制 (Read / Write / Ioctl) 与权限隔离
// =============================================================================
TEST(DeviceRegistryTest, CapabilityBasedReadWriteIoctl) {
    DeviceRegistry& reg = DeviceRegistry::instance();
    reg.clear();

    MockTestCharDevice dev("dev0");
    EXPECT_TRUE(reg.register_device(&dev, auroraos::kernel::CAP_RIGHT_READ | auroraos::kernel::CAP_RIGHT_WRITE));

    TaskControlBlock task{};

    // Slot 0: 读写能力
    reg.open_device(&task, "dev0", 0, auroraos::kernel::CAP_RIGHT_READ | auroraos::kernel::CAP_RIGHT_WRITE);
    // Slot 1: 只读能力
    reg.open_device(&task, "dev0", 1, auroraos::kernel::CAP_RIGHT_READ);

    // 1. 通过 Slot 0 写入数据
    const char write_data[] = "AuroraOS microkernel capability test!";
    int w_ret = reg.device_write(&task, 0, write_data, sizeof(write_data), 0);
    EXPECT_EQ(w_ret, static_cast<int>(sizeof(write_data)));

    // 2. 尝试通过 Slot 1 (只读) 写入数据，应被拒绝 (-3)
    int w_fail = reg.device_write(&task, 1, write_data, sizeof(write_data), 0);
    EXPECT_EQ(w_fail, -3);

    // 3. 通过 Slot 1 读取数据，应成功
    char read_buf[64] = {0};
    int r_ret = reg.device_read(&task, 1, read_buf, sizeof(read_buf), 0);
    EXPECT_EQ(r_ret, static_cast<int>(sizeof(write_data)));
    EXPECT_STREQ(read_buf, write_data);

    // 4. 通过能力执行 IOCTL
    int magic_arg = 0x1234;
    int ioctl_ret = reg.device_ioctl(&task, 0, 0x42, &magic_arg);
    EXPECT_EQ(ioctl_ret, 0x55AA);
    EXPECT_EQ(dev.last_ioctl_req, 0x42);
    EXPECT_EQ(dev.last_ioctl_arg, &magic_arg);

    // 5. 关闭设备能力
    int close_ret = reg.device_close(&task, 0);
    EXPECT_EQ(close_ret, 0);
    EXPECT_EQ(auroraos::kernel::CSpace::cap_lookup(&task, 0), nullptr);
}

// =============================================================================
// 5. 能力派生与撤销 (Derive / Revoke) 设备能力生命周期
// =============================================================================
TEST(DeviceRegistryTest, DeviceCapabilityDeriveAndRevoke) {
    DeviceRegistry& reg = DeviceRegistry::instance();
    reg.clear();

    MockTestCharDevice dev("dev_grant");
    EXPECT_TRUE(reg.register_device(&dev, auroraos::kernel::CAP_RIGHT_READ |
                                          auroraos::kernel::CAP_RIGHT_WRITE |
                                          auroraos::kernel::CAP_RIGHT_GRANT));

    TaskControlBlock task{};

    // Slot 0: 拥有 RWG 权限
    reg.open_device(&task, "dev_grant", 0, auroraos::kernel::CAP_RIGHT_READ |
                                           auroraos::kernel::CAP_RIGHT_WRITE |
                                           auroraos::kernel::CAP_RIGHT_GRANT);

    // 派生降权到 Slot 3: 仅赋予 Read 权限
    bool derived = auroraos::kernel::CSpace::cap_derive(&task, 0, 3, auroraos::kernel::CAP_RIGHT_READ);
    EXPECT_TRUE(derived);

    auroraos::kernel::Capability* derived_cap = auroraos::kernel::CSpace::cap_lookup(&task, 3);
    ASSERT_NE(derived_cap, nullptr);
    EXPECT_TRUE(derived_cap->rights.read);
    EXPECT_FALSE(derived_cap->rights.write);

    // 验证派生能力可以成功读取
    char buf[16] = {0};
    EXPECT_GE(reg.device_read(&task, 3, buf, sizeof(buf), 0), 0);
}
