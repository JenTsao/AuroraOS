#include <gtest/gtest.h>
#include "../../vfs/vfs.hpp"
#include "../../services/vfs/vfs_service.hpp"
#include "../../kernel/task/task.hpp"
#include "../../kernel/core/cspace.hpp"
#include "../../kernel/core/device.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#include <string>

using auroraos::vfs::VfsOpcode;
using auroraos::vfs::VfsReply;
using auroraos::vfs::VfsRequest;
using auroraos::vfs::VfsServer;

class FakeVNode : public VNode {
public:
    static constexpr int kCapacity = 128;

    int read(char* buf, int len, int offset, void* /*priv*/ = nullptr) override {
        if (offset < 0 || offset >= write_pos_)
            return 0;
        const int available = write_pos_ - offset;
        const int to_read = std::min(len, available);
        memcpy(buf, data_.data() + offset, static_cast<std::size_t>(to_read));
        return to_read;
    }

    int write(const char* buf, int len, int /*offset*/, void* /*priv*/ = nullptr) override {
        if (write_pos_ + len > kCapacity)
            return -1;
        memcpy(data_.data() + write_pos_, buf, static_cast<std::size_t>(len));
        write_pos_ += len;
        return len;
    }

    int get_size(void* /*priv*/ = nullptr) const override {
        return write_pos_;
    }

    const char* raw_data() const noexcept {
        return data_.data();
    }

private:
    std::array<char, kCapacity> data_{};
    int write_pos_{0};
};

class VfsTest : public ::testing::Test {
protected:
    void SetUp() override {
        VfsServer::instance().init();
        vnode_ = std::make_unique<FakeVNode>();
    }

    bool mount(const char* path = "/dev/test") {
        return VfsServer::instance().mount(path, vnode_.get());
    }

    int open_file(const char* path, int flags = 0) {
        VfsRequest req;
        req.opcode = VfsOpcode::Open;
        strncpy(req.open.path, path, sizeof(req.open.path) - 1);
        req.open.flags = flags;
        VfsReply reply;
        VfsServer::instance().process_request(req, reply);
        return reply.status;
    }

    int read_file(int fd, char* buf, int len) {
        VfsRequest req;
        req.opcode = VfsOpcode::Read;
        req.fd = fd;
        req.read.len = len;
        VfsReply reply;
        VfsServer::instance().process_request(req, reply);
        if (reply.status > 0) {
            memcpy(buf, reply.read.data, reply.status);
        }
        return reply.status;
    }

    int write_file(int fd, const char* buf, int len) {
        VfsRequest req;
        req.opcode = VfsOpcode::Write;
        req.fd = fd;
        req.write.len = len;
        memcpy(req.write.data, buf, len);
        VfsReply reply;
        VfsServer::instance().process_request(req, reply);
        return reply.status;
    }

    int lseek_file(int fd, int offset, int whence) {
        VfsRequest req;
        req.opcode = VfsOpcode::Lseek;
        req.fd = fd;
        req.lseek.offset = offset;
        req.lseek.whence = whence;
        VfsReply reply;
        VfsServer::instance().process_request(req, reply);
        return reply.status;
    }

    int close_file(int fd) {
        VfsRequest req;
        req.opcode = VfsOpcode::Close;
        req.fd = fd;
        VfsReply reply;
        VfsServer::instance().process_request(req, reply);
        return reply.status;
    }

    int ioctl_file(int fd, int request, void* arg) {
        VfsRequest req;
        req.opcode = VfsOpcode::Ioctl;
        req.fd = fd;
        req.ioctl.request = request;
        req.ioctl.arg = arg;
        VfsReply reply;
        VfsServer::instance().process_request(req, reply);
        return reply.status;
    }

    std::unique_ptr<FakeVNode> vnode_;
};

TEST_F(VfsTest, MountAndOpen) {
    ASSERT_TRUE(mount());
    int fd = open_file("/dev/test");
    EXPECT_GE(fd, 0) << "open() on a mounted path must return a valid fd";
}

TEST_F(VfsTest, OpenNotMounted) {
    int fd = open_file("/does/not/exist");
    EXPECT_EQ(fd, -1);
}

TEST_F(VfsTest, ReadWriteBasic) {
    ASSERT_TRUE(mount());
    int fd = open_file("/dev/test", 3);
    ASSERT_GE(fd, 0);

    const char* msg = "hello aurora";
    int len = strlen(msg);
    EXPECT_EQ(write_file(fd, msg, len), len);
    EXPECT_EQ(lseek_file(fd, 0, 0), 0);

    char buf[32]{};
    EXPECT_EQ(read_file(fd, buf, len), len);
    EXPECT_STREQ(buf, msg);
    EXPECT_EQ(close_file(fd), 0);
}

TEST_F(VfsTest, MaxOpenFiles) {
    ASSERT_TRUE(mount());
    std::vector<int> fds(VfsServer::MAX_OPEN_FILES);
    for (int i = 0; i < VfsServer::MAX_OPEN_FILES; ++i) {
        fds[i] = open_file("/dev/test");
        EXPECT_GE(fds[i], 0);
    }

    int overflow_fd = open_file("/dev/test");
    EXPECT_EQ(overflow_fd, -1);

    for (int fd : fds) {
        if (fd >= 0)
            close_file(fd);
    }
}

TEST_F(VfsTest, MountAndUnmount) {
    ASSERT_TRUE(mount("/dev/storage"));
    int fd = open_file("/dev/storage");
    EXPECT_GE(fd, 0);
    EXPECT_EQ(close_file(fd), 0);

    // Unmount
    VfsRequest req;
    req.opcode = VfsOpcode::Unmount;
    strncpy(req.unmount.path, "/dev/storage", sizeof(req.unmount.path) - 1);
    VfsReply reply;
    VfsServer::instance().process_request(req, reply);
    EXPECT_EQ(reply.status, 0);

    // Opening unmounted path should now fail
    int fd2 = open_file("/dev/storage");
    EXPECT_EQ(fd2, -1);
}

// 安全回归：IPC 请求路径禁止携带内核 VNode* 指针（防止伪造指针注入内核）
TEST_F(VfsTest, MountViaIpcRequestRejected) {
    VfsRequest req;
    memset(&req, 0, sizeof(req));
    req.opcode = VfsOpcode::Mount;
    strncpy(req.mount.path, "/dev/evil", sizeof(req.mount.path) - 1);
    req.mount.vnode_ptr = vnode_.get(); // 攻击者可控的裸指针

    VfsReply reply;
    VfsServer::instance().process_request(req, reply);
    EXPECT_EQ(reply.status, -1) << "Mount must not be accepted through the IPC request path";

    // 注入的挂载点不得生效
    int fd = open_file("/dev/evil");
    EXPECT_EQ(fd, -1);
}

// ============================================================
// 权限检查（caller_authorized）—— 跨信任域 IPC 调用路径的安全回归。
// 真实 caller_id 走能力判定：fail-closed / 内核特权旁路 /
// Device 能力「对象 + 权利」匹配 / 撤销即时生效。
// ============================================================

namespace {

// 可读写的小设备节点：Device 以双继承身份挂进 VFS（KernelObject 主基类 +
// VNode），与固件侧 /dev/* 设备的挂载方式一致，保证能力 object 与挂载
// VNode 的地址可比（主基类地址 ≠ VNode 子对象地址，必须经类型对齐）。
class PermDevice : public Device {
public:
    PermDevice() : Device("permdev", DeviceType::Char) {}

    int read(char* buf, int len, int offset, void* /*priv*/) override {
        if (offset < 0 || offset >= static_cast<int>(sizeof(storage_)))
            return 0;
        int n = len;
        if (n > static_cast<int>(sizeof(storage_)) - offset)
            n = static_cast<int>(sizeof(storage_)) - offset;
        memcpy(buf, storage_ + offset, static_cast<std::size_t>(n));
        return n;
    }

    int write(const char* buf, int len, int offset, void* /*priv*/) override {
        if (offset < 0 || len < 0 || offset + len > static_cast<int>(sizeof(storage_)))
            return -1;
        memcpy(storage_ + offset, buf, static_cast<std::size_t>(len));
        return len;
    }

private:
    char storage_[64] = "seed";
};

} // namespace

class VfsPermissionTest : public ::testing::Test {
protected:
    void SetUp() override {
        VfsServer::instance().init();
        Scheduler::instance().init();
        device_ = std::make_unique<PermDevice>();
        ASSERT_TRUE(VfsServer::instance().mount("/dev/perm0", device_.get()));

        // 静态栈：TCB 数组由调度器静态持有，栈空间须与 TCB 同生命周期。
        static uint32_t user_stack[128];
        user_task_ = Scheduler::instance().create_task([]() {}, user_stack, sizeof(user_stack),
                                                       TaskPriority::Normal, 0, TaskPrivilege::User);
        ASSERT_NE(user_task_, nullptr);

        static uint32_t kernel_stack[128];
        kernel_task_ = Scheduler::instance().create_task([]() {}, kernel_stack, sizeof(kernel_stack));
        ASSERT_NE(kernel_task_, nullptr);
    }

    uint32_t user_id() const {
        return user_task_->scheduler.id;
    }

    uint32_t kernel_id() const {
        return kernel_task_->scheduler.id;
    }

    // 向用户任务槽位 0 铸造指向 device_ 的设备能力
    void grant_device_cap(bool read, bool write) {
        auroraos::kernel::Capability cap{};
        cap.type = auroraos::kernel::CapType::Device;
        cap.rights.read = read;
        cap.rights.write = write;
        cap.object = device_.get();
        ASSERT_TRUE(auroraos::kernel::CSpace::cap_insert(user_task_, 0, cap));
    }

    int open_as(uint32_t caller, const char* path, int flags = 0) {
        VfsRequest req;
        memset(&req, 0, sizeof(req));
        req.opcode = VfsOpcode::Open;
        strncpy(req.open.path, path, sizeof(req.open.path) - 1);
        req.open.flags = flags;
        VfsReply reply;
        VfsServer::instance().process_request(req, reply, caller);
        return reply.status;
    }

    int read_as(uint32_t caller, int fd, char* buf, int len) {
        VfsRequest req;
        memset(&req, 0, sizeof(req));
        req.opcode = VfsOpcode::Read;
        req.fd = fd;
        req.read.len = len;
        VfsReply reply;
        VfsServer::instance().process_request(req, reply, caller);
        if (reply.status > 0)
            memcpy(buf, reply.read.data, static_cast<std::size_t>(reply.status));
        return reply.status;
    }

    int write_as(uint32_t caller, int fd, const char* buf, int len) {
        VfsRequest req;
        memset(&req, 0, sizeof(req));
        req.opcode = VfsOpcode::Write;
        req.fd = fd;
        req.write.len = len;
        memcpy(req.write.data, buf, static_cast<std::size_t>(len));
        VfsReply reply;
        VfsServer::instance().process_request(req, reply, caller);
        return reply.status;
    }

    int close_file(int fd) {
        VfsRequest req;
        req.opcode = VfsOpcode::Close;
        req.fd = fd;
        VfsReply reply;
        VfsServer::instance().process_request(req, reply);
        return reply.status;
    }

    int lseek_file(int fd, int offset, int whence) {
        VfsRequest req;
        req.opcode = VfsOpcode::Lseek;
        req.fd = fd;
        req.lseek.offset = offset;
        req.lseek.whence = whence;
        VfsReply reply;
        VfsServer::instance().process_request(req, reply);
        return reply.status;
    }

    std::unique_ptr<PermDevice> device_;
    TaskControlBlock* user_task_ = nullptr;
    TaskControlBlock* kernel_task_ = nullptr;
};

// 进程内直连（默认 NO_IPC_CALLER）不引入跨信任域输入，必须保持放行
TEST_F(VfsPermissionTest, DirectCallerBypassRetained) {
    int fd = open_as(VfsServer::NO_IPC_CALLER, "/dev/perm0");
    EXPECT_GE(fd, 0);
    EXPECT_EQ(close_file(fd), 0);
}

// fail-closed：查不到 TCB 的 caller_id 一律拒绝
TEST_F(VfsPermissionTest, UnknownCallerFailClosed) {
    EXPECT_EQ(open_as(9999u, "/dev/perm0"), -1);
}

// 用户任务未持有任何能力 → 打开被拒
TEST_F(VfsPermissionTest, UserTaskWithoutCapDenied) {
    EXPECT_EQ(open_as(user_id(), "/dev/perm0"), -1);
}

// 只读能力：可打开读取；写请求与写打开均被拒
TEST_F(VfsPermissionTest, ReadOnlyCapAllowsReadDeniesWrite) {
    grant_device_cap(/*read=*/true, /*write=*/false);

    int fd = open_as(user_id(), "/dev/perm0", 0); // O_RDONLY
    ASSERT_GE(fd, 0);

    char buf[16]{};
    EXPECT_GT(read_as(user_id(), fd, buf, sizeof(buf)), 0);

    const char msg[] = "nope";
    EXPECT_EQ(write_as(user_id(), fd, msg, 4), -1) << "write with read-only cap must be denied";

    int wfd = open_as(user_id(), "/dev/perm0", 1); // O_WRONLY
    EXPECT_EQ(wfd, -1) << "open-for-write with read-only cap must be denied";
}

// 读写能力：读写均放行
TEST_F(VfsPermissionTest, ReadWriteCapAllowsBoth) {
    grant_device_cap(/*read=*/true, /*write=*/true);

    int fd = open_as(user_id(), "/dev/perm0", 2); // O_RDWR
    ASSERT_GE(fd, 0);

    const char msg[] = "aurora";
    EXPECT_EQ(write_as(user_id(), fd, msg, 6), 6);

    char buf[16]{};
    ASSERT_GE(read_as(user_id(), fd, buf, 6), 0);
    EXPECT_EQ(lseek_file(fd, 0, 0), 0);
    memset(buf, 0, sizeof(buf));
    EXPECT_EQ(read_as(user_id(), fd, buf, 6), 6);
    EXPECT_EQ(memcmp(buf, msg, 6), 0);
}

// 内核特权旁路：不持有任何能力的内核任务（ui_render_task 契约）可访问设备
TEST_F(VfsPermissionTest, KernelPrivilegeBypassWithoutCaps) {
    int fd = open_as(kernel_id(), "/dev/perm0", 0);
    EXPECT_GE(fd, 0);
    char buf[16]{};
    EXPECT_GT(read_as(kernel_id(), fd, buf, sizeof(buf)), 0);
}

// 操作时重验：能力撤销后已打开的 fd 立即不可用
TEST_F(VfsPermissionTest, CapRevokeTakesEffectAtOperationTime) {
    grant_device_cap(/*read=*/true, /*write=*/false);

    int fd = open_as(user_id(), "/dev/perm0", 0);
    ASSERT_GE(fd, 0);

    ASSERT_TRUE(auroraos::kernel::CSpace::cap_delete(user_task_, 0));

    char buf[16]{};
    EXPECT_EQ(read_as(user_id(), fd, buf, sizeof(buf)), -1)
        << "read after cap revocation must be denied";
}

// fd 表全局共享：无能力的第三方调用方借用他人 fd 必须被拒
TEST_F(VfsPermissionTest, ForeignCallerCannotUseForeignFd) {
    grant_device_cap(/*read=*/true, /*write=*/true);
    int fd = open_as(user_id(), "/dev/perm0", 2);
    ASSERT_GE(fd, 0);

    // 内核任务虽有旁路，此处改用「无能力的另一个用户任务」验证隔离。
    static uint32_t other_stack[128];
    TaskControlBlock* other = Scheduler::instance().create_task([]() {}, other_stack, sizeof(other_stack),
                                                                TaskPriority::Normal, 0, TaskPrivilege::User);
    ASSERT_NE(other, nullptr);

    char buf[16]{};
    EXPECT_EQ(read_as(other->scheduler.id, fd, buf, sizeof(buf)), -1);
    EXPECT_EQ(write_as(other->scheduler.id, fd, "x", 1), -1);
}

