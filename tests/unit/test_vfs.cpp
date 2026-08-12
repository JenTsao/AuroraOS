#include <gtest/gtest.h>
#include "../../vfs/vfs.hpp"
#include "../../services/vfs/vfs_service.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#include <string>

using auroraos::vfs::VfsServer;
using auroraos::vfs::VfsRequest;
using auroraos::vfs::VfsReply;
using auroraos::vfs::VfsOpcode;

class FakeVNode : public VNode {
public:
    static constexpr int kCapacity = 128;

    int read(char* buf, int len, int offset, void* /*priv*/ = nullptr) override {
        if (offset < 0 || offset >= write_pos_) return 0;
        const int available = write_pos_ - offset;
        const int to_read   = std::min(len, available);
        std::memcpy(buf, data_.data() + offset, static_cast<std::size_t>(to_read));
        return to_read;
    }

    int write(const char* buf, int len, int /*offset*/, void* /*priv*/ = nullptr) override {
        if (write_pos_ + len > kCapacity) return -1;
        std::memcpy(data_.data() + write_pos_, buf, static_cast<std::size_t>(len));
        write_pos_ += len;
        return len;
    }

    int get_size(void* /*priv*/ = nullptr) const override { return write_pos_; }
    const char* raw_data() const noexcept { return data_.data(); }

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
        std::strncpy(req.open.path, path, sizeof(req.open.path) - 1);
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
            std::memcpy(buf, reply.read.data, reply.status);
        }
        return reply.status;
    }

    int write_file(int fd, const char* buf, int len) {
        VfsRequest req;
        req.opcode = VfsOpcode::Write;
        req.fd = fd;
        req.write.len = len;
        std::memcpy(req.write.data, buf, len);
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
    int len = std::strlen(msg);
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
        if (fd >= 0) close_file(fd);
    }
}
